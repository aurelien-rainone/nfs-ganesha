/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright © Cynny Space, 2017
 * Author: Aurélien Rainone <a.rainone@cynnyspace.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

/**
 * @file   s3_methods.c
 * @brief S3 definitions for the S3 FSAL
 *
 * This file includes definitions of S3 specific functions
 * for the S3 FSAL.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"
#include "s3_methods.h"


static const int showResponsePropertiesG = 1;
static int statusG = 0;
static char errorDetailsG[4096] = { 0 };


static S3Status printError()
{
	if (statusG < S3StatusErrorAccessDenied) {
		fprintf(stderr, "\nERROR: %s\n", S3_get_status_name(statusG));
	}
	else {
		fprintf(stderr, "\nERROR: %s\n", S3_get_status_name(statusG));
		fprintf(stderr, "%s\n", errorDetailsG);
	}

	return statusG;
}

static int should_retry(int num_retries, int interval)
{
	if (num_retries--) {
		// Sleep before next retry; start out with a 1 second sleep
		sleep(interval);
		return 1;
	}
	return 0;
}


S3Status responsePropertiesCallback
(const S3ResponseProperties *properties, void *callbackData)
{
	(void) callbackData;

	if (!showResponsePropertiesG) {
		return S3StatusOK;
	}

#define print_nonnull(name, field)					\
	do {								\
		if (properties-> field) {				\
			printf("%s: %s\n", name, properties-> field);	\
		}							\
	} while (0)

	print_nonnull("Content-Type", contentType);
	print_nonnull("Request-Id", requestId);
	print_nonnull("Request-Id-2", requestId2);
	if (properties->contentLength > 0) {
		printf("Content-Length: %llu\n",
		       (unsigned long long) properties->contentLength);
	}
	print_nonnull("Server", server);
	print_nonnull("ETag", eTag);
	if (properties->lastModified > 0) {
		char timebuf[256];
		time_t t = (time_t) properties->lastModified;
		// gmtime is not thread-safe but we don't care here.
		strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
		printf("Last-Modified: %s\n", timebuf);
	}
	int i;
	for (i = 0; i < properties->metaDataCount; i++) {
		printf("x-amz-meta-%s: %s\n", properties->metaData[i].name,
		       properties->metaData[i].value);
	}
	if (properties->usesServerSideEncryption) {
		printf("UsesServerSideEncryption: true\n");
	}

	return S3StatusOK;
}


// response complete callback ------------------------------------------------

// This callback does the same thing for every request type: saves the status
// and error stuff in global variables
void responseCompleteCallback(S3Status status,
				     const S3ErrorDetails *error,
				     void *callbackData)
{
	(void) callbackData;

	statusG = status;
	// Compose the error details message now, although we might not use it.
	// Can't just save a pointer to [error] since it's not guaranteed to last
	// beyond this callback
	int len = 0;
	if (error && error->message) {
	len += snprintf(&(errorDetailsG[len]), sizeof(errorDetailsG) - len,
			"  Message: %s\n", error->message);
	}
	if (error && error->resource) {
		len += snprintf(&(errorDetailsG[len]), sizeof(errorDetailsG) - len,
				"  Resource: %s\n", error->resource);
	}
	if (error && error->furtherDetails) {
	len += snprintf(&(errorDetailsG[len]), sizeof(errorDetailsG) - len,
			  "  Further Details: %s\n", error->furtherDetails);
	}
	if (error && error->extraDetailsCount) {
		len += snprintf(&(errorDetailsG[len]), sizeof(errorDetailsG) - len,
				"%s", "  Extra Details:\n");
	int i;
	for (i = 0; i < error->extraDetailsCount; i++) {
		len += snprintf(&(errorDetailsG[len]),
				sizeof(errorDetailsG) - len, "    %s: %s\n",
				error->extraDetails[i].name,
				error->extraDetails[i].value);
	}
	}
}

/**
 * This callbacks saves the status and log (in debug) extra informations
 */
void s3_resp_complete_cb(S3Status status,
			 const S3ErrorDetails *error,
			 void *callbackData)
{
	struct s3_response_callback_data *s3cbdata;
	s3cbdata = (struct s3_response_callback_data*) (callbackData);
	s3cbdata->status = status;

	if (status == S3StatusOK) {
		LogDebug(COMPONENT_FSAL, "Successful request, res=%s", error->resource);
	} else {
		if (error && error->message) {
			LogDebug(COMPONENT_FSAL, "Message: %s", error->message);
		}
		if (error && error->resource) {
			LogDebug(COMPONENT_FSAL, "Resource: %s", error->resource);
		}
		if (error && error->furtherDetails) {
			LogDebug(COMPONENT_FSAL, "Further details: %s", error->furtherDetails);
		}
		if (error && error->extraDetailsCount) {
			int i;
			for (i = 0; i < error->extraDetailsCount; i++) {
				LogDebug(COMPONENT_FSAL, "Extra details: %s->%s",
				error->extraDetails[i].name, error->extraDetails[i].value);
			}
		}
	}
}


/**
 *		~~~ list bucket ~~~
 */

typedef struct list_bucket_callback_data
{
	int isTruncated;
	char *prefix;		/*< request prefix */
	char nextMarker[1024];
	int keyCount;
	int allDetails;
	struct s3_fsal_obj_handle *parent;
} list_bucket_callback_data;


static S3Status listBucketCallback(int isTruncated, const char *nextMarker,
				   int contentsCount,
				   const S3ListBucketContent *contents,
				   int commonPrefixesCount,
				   const char **commonPrefixes,
				   void *callbackData)
{
	list_bucket_callback_data *data = (list_bucket_callback_data *) callbackData;

	data->isTruncated = isTruncated;
	// This is tricky.  S3 doesn't return the NextMarker if there is no
	// delimiter.  Why, I don't know, since it's still useful for paging
	// through results.  We want NextMarker to be the last content in the
	// list, so set it to that if necessary.
	if ((!nextMarker || !nextMarker[0]) && contentsCount) {
		nextMarker = contents[contentsCount - 1].key;
	}
	if (nextMarker) {
		snprintf(data->nextMarker, sizeof(data->nextMarker), "%s",
			 nextMarker);
	} else {
		data->nextMarker[0] = 0;
	}

	printf("listBucketCallback response:\n");

	char timebuf[256];
	char *filename = gsh_malloc(S3_MAX_KEY_SIZE);
	int i;
	for (i = 0; i < contentsCount; i++) {
		const S3ListBucketContent *content = &(contents[i]);

		time_t t = (time_t) content->lastModified;
		strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ",
			 gmtime(&t));

		/** extract filename from the full key name by removing the
		 *  common prefix used for the request */
		if (data->prefix) {
			if (strstr(content->key, data->prefix)) {
				strcpy(filename, content->key + strlen(data->prefix));
				printf("filename = %s\n", filename);
			}
		} else {
			/* copy the whole key name */
			strncpy(filename, content->key, S3_MAX_KEY_SIZE);
		}

		printf("\nKey: %s (filename=%s)\n", content->key, filename);
		printf("Last Modified: %s\n", timebuf);
		printf("ETag: %s\n", content->eTag);
		printf("Size: %llu\n", (unsigned long long) content->size);
		if (content->ownerId) {
			printf("Owner ID: %s\n", content->ownerId);
		}
		if (content->ownerDisplayName) {
			printf("Owner Display Name: %s\n", content->ownerDisplayName);
		}

		// check if a dirent for this key already exists
		const struct s3_dirent *ent = s3_dirent_lookup(data->parent, filename);
		if (ent) {
			// object already exists
			// TODO: check if it is of the same type (file/dir) and update attributes
			LogDebug(COMPONENT_FSAL, "dirent for %s already exists", filename);
		} else {
			// create and fill a new handle corresponding to this key
			struct fsal_obj_handle *new_obj;
			struct attrlist attrs_in, attrs_out;
			fsal_status_t status = {0, 0};

			attrs_in.mode = 0755;
			attrs_in.valid_mask = ATTR_MODE;
			attrs_in.filesize = content->size;
			attrs_in.spaceused = content->size;
			attrs_in.valid_mask |= ATTR_SIZE;
			attrs_in.mtime.tv_sec = content->lastModified;
			attrs_in.valid_mask |= ATTR_MTIME;

			LogFullDebug(COMPONENT_FSAL,
				     "listBucketCallback, s3_create_obj(FILE), fullname=%s, name=%s",
				      content->key, filename);

			status = s3_create_obj(data->parent, REGULAR_FILE, filename, &attrs_in,
					  &new_obj, &attrs_out);
			if (FSAL_IS_ERROR(status)) {
				LogCrit(COMPONENT_FSAL,
				"listBucketCallback, s3_create_obj(FILE) error, fullname=%s, name=%s status=%s",
				content->key, filename, msg_fsal_err(status.major));
			}
		}
	}

	for (i = 0; i < commonPrefixesCount; i++) {
		printf("\nCommon Prefix: %s\n", commonPrefixes[i]);

		/** extract filename from the full key name by removing the
		 *  common prefix used for the request
		 *  -1 to remove the trailing '/' */
		if (data->prefix) {
			if (strstr(commonPrefixes[i], data->prefix)) {
				strcpy(filename, commonPrefixes[i] + strlen(data->prefix) - 1);
				printf("filename = %s\n", filename);
			}
		} else {
			strncpy(filename, commonPrefixes[i], S3_MAX_KEY_SIZE);
			filename[strlen(filename) - 1] = '\0'; /* remove trailing slash */
		}

		// check if a dirent for this dir already exists
		const struct s3_dirent *ent = s3_dirent_lookup(data->parent, commonPrefixes[i]);
		if (ent) {
			// object already exists
			// TODO: check if it is of the same type (file/dir) and update attributes
			LogDebug(COMPONENT_FSAL, "dirent for %s already exists", commonPrefixes[i]);
		} else {
			// create and fill a new handle corresponding to this key
			struct fsal_obj_handle *new_obj;
			struct attrlist attrs_in, attrs_out;
			fsal_status_t status = {0, 0};

			attrs_in.mode = 0755;
			attrs_in.valid_mask = ATTR_MODE;

			LogFullDebug(COMPONENT_FSAL,
				     "listBucketCallback, s3_create_obj(DIR), fullname=%s, name=%s",
				      commonPrefixes[i], filename);

			status = s3_create_obj(data->parent, DIRECTORY, filename, &attrs_in,
					  &new_obj, &attrs_out);
			if (FSAL_IS_ERROR(status)) {
				LogCrit(COMPONENT_FSAL,
				"listBucketCallback, s3_create_obj(DIR) error, fullname=%s, name=%s status=%s",
				commonPrefixes[i], filename, msg_fsal_err(status.major));
			}
		}
	}
	gsh_free(filename);

	return S3StatusOK;
}

S3Status list_bucket(const S3BucketContext *bucketContext, struct s3_fsal_obj_handle *parent,
		     const char *prefix, const char *marker, const char *delimiter,
		     int maxkeys, int allDetails)
{
	S3ListBucketHandler listBucketHandler = {
		{ &responsePropertiesCallback, &responseCompleteCallback },
		&listBucketCallback
	};

	list_bucket_callback_data data;

	if (marker) {
		snprintf(data.nextMarker, sizeof(data.nextMarker), "%s", marker);
	} else {
		data.nextMarker[0] = 0;
	}
	data.keyCount = 0;
	data.allDetails = allDetails;
	data.parent = parent;
	data.prefix = (char*) prefix;

	// setup retry variables
	int num_retries = S3.max_retries;
	int interval = S3.sleep_interval;

	printf("S3_list_bucket: %s prefix=%s delimiter=%s marker=%s \n",
	       bucketContext->bucketName, prefix, delimiter, marker);

	do {
		data.isTruncated = 0;
		do {
			S3_list_bucket(bucketContext, prefix, data.nextMarker,
				       delimiter, maxkeys,
				       0, S3.request_timeout,
				       &listBucketHandler, &data);
			// decrement retries and wait 1 second longer
			--num_retries;
			++interval;
		} while (S3_status_is_retryable(statusG)
			 && should_retry(num_retries, interval));
		if (statusG != S3StatusOK) {
			break;
		}
	} while (data.isTruncated && (!maxkeys || (data.keyCount < maxkeys)));

	if (statusG != S3StatusOK) {
		return printError();
	}

	return S3StatusOK;
}

/**
 *		~~~ test bucket ~~~
 */

void test_bucket(const S3BucketContext *ctx, struct s3_response_callback_data *cbdata)
{
	S3ResponseHandler responseHandler = {
		&responsePropertiesCallback, &s3_resp_complete_cb
	};

	char locationConstraint[64];

	// setup retry variables
	int num_retries = S3.max_retries;
	int interval = S3.sleep_interval;

	printf("S3_test_bucket: %s\n", ctx->bucketName);

	do {
		S3_test_bucket(ctx->protocol, ctx->uriStyle,
			       ctx->accessKeyId, ctx->secretAccessKey,
			       0, 0, ctx->bucketName, ctx->authRegion,
			       sizeof(locationConstraint), locationConstraint,
			       0, S3.request_timeout, &responseHandler, cbdata);
	// decrement retries and wait 1 second longer
	--num_retries;
	++interval;
	} while (S3_status_is_retryable(cbdata->status) && should_retry(num_retries, interval));

	const char *result;

	switch (cbdata->status) {
		case S3StatusOK:
			// bucket exists
			result = locationConstraint[0] ? locationConstraint : "USA";
			break;
		case S3StatusErrorNoSuchBucket:
			result = "Does Not Exist";
			break;
		case S3StatusErrorAccessDenied:
			result = "Access Denied";
			break;
		default:
			result = 0;
			break;
	}

	if (result) {
		printf("%-56s  %-20s\n", "                         Bucket",
		       "       Status");
	printf("--------------------------------------------------------  "
	       "--------------------\n");
	printf("%-56s  %-20s\n", ctx->bucketName, result);
	}
	else {
		printError();
	}
}

