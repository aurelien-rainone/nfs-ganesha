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
 * @file   s3_methods.h
 * @brief S3 declarations for the S3 FSAL
 *
 * This file includes declarations of S3 specific functions
 * for the S3 FSAL.
 */

#ifndef FSAL_S3_S3_METHODS
#define FSAL_S3_S3_METHODS

#include <libs3.h>
#include "internal.h"

/** forward declarations */
struct s3_fsal_obj_handle;

struct s3_response_callback_data {
	/*< request status */
	int status;
};

/**
 * response properties callback
 * This callback does the same thing for every request type: prints out the
 * properties if the user has requested them to be so
 */
S3Status responsePropertiesCallback(const S3ResponseProperties *properties,
				    void *callbackData);

/**
 * response complete callback
 * This callback does the same thing for every request type: saves the status
 * and error stuff in global variables
 */
void responseCompleteCallback(S3Status status,
			      const S3ErrorDetails *error,
			      void *callbackData);

S3Status list_bucket(const S3BucketContext *bucketContext,
		     struct s3_fsal_obj_handle *parent, const char *prefix,
		     const char *marker, const char *delimiter, int maxkeys,
		     int allDetails);

void test_bucket(const S3BucketContext *ctx,
		 struct s3_response_callback_data *cbdata);

#endif				/* !FSAL_S3_S3_METHODS */
