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
 * @file   internal.c
 * @brief Internal definitions for the S3 FSAL
 *
 * This file includes internal function definitions, constants, and
 * variable declarations used to implement the S3 FSAL, but not
 * exposed as part of the API.
 */

/* export.c
 * S3 FSAL export object
 */

#include <limits.h>
#include <stdint.h>
#include <sys/statvfs.h>
#include "abstract_mem.h"
#include "fsal.h"
#include "fsal_types.h"
#include "fsal_api.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_config.h"
#include "internal.h"

/* helpers to/from other S3 objects
 */

struct fsal_staticfsinfo_t *s3_staticinfo(struct fsal_module *hdl);

/**
 * @brief Clean up an export
 *
 * This function cleans up an export after the last reference is
 * released.
 *
 * @param[in,out] exp_hdl The export to be released
 *
 * @retval ERR_FSAL_NO_ERROR on success.
 * @retval ERR_FSAL_BUSY if the export is in use.
 */

static void s3_release_export(struct fsal_export *exp_hdl)
{
	struct s3_fsal_export *myself;

	myself = container_of(exp_hdl, struct s3_fsal_export, export);

	if (myself->root_handle != NULL) {
		s3_clean_export(myself->root_handle);

		fsal_obj_handle_fini(&myself->root_handle->obj_handle);

		LogDebug(COMPONENT_FSAL,
			 "Releasing hdl=%p, name=%s",
			 myself->root_handle, myself->root_handle->m_name);

		/*PTHREAD_RWLOCK_wrlock(&myself->mfe_exp_lock);*/
		s3_free_handle(myself->root_handle);
		/*PTHREAD_RWLOCK_unlock(&myself->mfe_exp_lock);*/

		myself->root_handle = NULL;
	}

	fsal_detach_export(exp_hdl->fsal, &exp_hdl->exports);
	free_export_ops(exp_hdl);

	/*glist_del(&myself->export_entry);*/

	/*gsh_free(myself->export_path);*/
	gsh_free(myself);

	/* release libs3 */
	S3_deinitialize();
}

/**
 * @brief Get dynamic filesystem info
 *
 * This function returns dynamic filesystem information for the given
 * export.
 *
 * @param[in]  export_pub The public export handle
 * @param[out] info       The dynamic FS information
 *
 * @return FSAL status.
 */
static fsal_status_t s3_get_dynamic_info(struct fsal_export *export_pub,
					 struct fsal_obj_handle *obj_hdl,
					 fsal_dynamicfsinfo_t *infop)
{
	infop->total_bytes = 0;
	infop->free_bytes = 0;
	infop->avail_bytes = 0;
	infop->total_files = 0;
	infop->free_files = 0;
	infop->avail_files = 0;
	infop->time_delta.tv_sec = 1;
	infop->time_delta.tv_nsec = 0;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}


/**
 * @brief Query the FSAL's capabilities
 *
 * This function queries the capabilities of an FSAL export.
 *
 * @param[in] export_pub The public export handle
 * @param[in] option     The option to check
 *
 * @retval true if the option is supported.
 * @retval false if the option is unsupported (or unknown).
 */

static bool fs_supports(struct fsal_export *export_pub,
			fsal_fsinfo_options_t option)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_supports(info, option);}

/**
 * @brief Return the longest file supported
 *
 * This function returns the length of the longest file supported.
 *
 * @param[in] export_pub The public export
 *
 * @return the maximum filesize.
 */

static uint64_t fs_maxfilesize(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_maxfilesize(info);
}

/**
 * @brief Return the longest read supported
 *
 * This function returns the length of the longest read supported.
 *
 * @param[in] export_pub The public export
 *
 * @return the maximum read
 */

static uint32_t fs_maxread(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_maxread(info);
}

/**
 * @brief Return the longest write supported
 *
 * This function returns the length of the longest write supported.
 *
 * @param[in] export_pub The public export
 *
 * @return 4 mebibytes.
 */

static uint32_t fs_maxwrite(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_maxwrite(info);
}

/**
 * @brief Return the maximum number of hard links to a file
 *
 * This function returns the maximum number of hard links supported to
 * any file.
 *
 * @param[in] export_pub The public export
 *
 * @return max number of hard links to a file
 */

static uint32_t fs_maxlink(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_maxlink(info);
}

/**
 * @brief Return the maximum size of a Ceph filename
 *
 * This function returns the maximum filename length.
 *
 * @param[in] export_pub The public export
 *
 * @return UINT32_MAX.
 */

static uint32_t fs_maxnamelen(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_maxnamelen(info);
}

/**
 * @brief Return the maximum length of a Ceph path
 *
 * This function returns the maximum path length.
 *
 * @param[in] export_pub The public export
 *
 * @return UINT32_MAX.
 */

static uint32_t fs_maxpathlen(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_maxpathlen(info);
}

/**
 * @brief Return the lease time
 *
 * This function returns the lease time.
 *
 * @param[in] export_pub The public export
 *
 * @return five minutes.
 */

static struct timespec fs_lease_time(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_lease_time(info);
}

/**
 * @brief Return ACL support
 *
 * This function returns the export's ACL support.
 *
 * @param[in] export_pub The public export
 *
 * @return FSAL_ACLSUPPORT_DENY.
 */

static fsal_aclsupp_t fs_acl_support(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_acl_support(info);
}

/**
 * @brief Return the attributes supported by this FSAL
 *
 * This function returns the mask of attributes this FSAL can support.
 *
 * @param[in] export_pub The public export
 *
 * @return supported_attributes as defined in internal.c.
 */

static attrmask_t fs_supported_attrs(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_supported_attrs(info);
}

/**
 * @brief Return the mode under which the FSAL will create files
 *
 * This function modifies the default mode on any new file created.
 *
 * @param[in] export_pub The public export
 *
 * @return 0 (usually).  Bits set here turn off bits in created files.
 */

static uint32_t fs_umask(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_umask(info);
}

/**
 * @brief Return the mode for extended attributes
 *
 * This function returns the access mode applied to extended
 * attributes.  Dubious.
 *
 * @param[in] export_pub The public export
 *
 * @return 0644.
 */

static uint32_t fs_xattr_access_rights(struct fsal_export *export_pub)
{
	struct fsal_staticfsinfo_t *info;

	info = s3_staticinfo(export_pub->fsal);
	return fsal_xattr_access_rights(info);
}

/**
 * @brief Decode a digested handle
 *
 * This function decodes a previously digested handle.
 *
 * @param[in]  exp_handle  Handle of the relevant fs export
 * @param[in]  in_type  The type of digest being decoded
 * @param[out] fh_desc  Address and length of key
 */
static fsal_status_t s3_wire_to_host(struct fsal_export *exp_hdl,
				  fsal_digesttype_t in_type,
				  struct gsh_buffdesc *fh_desc,
				  int flags)
{
	size_t fh_min;
	uint64_t *hashkey;
	ushort *len;

	fh_min = 1;

	if (fh_desc->len < fh_min) {
		LogMajor(COMPONENT_FSAL,
			 "Size mismatch for handle.  should be >= %zu, got %zu",
			 fh_min, fh_desc->len);
		return fsalstat(ERR_FSAL_SERVERFAULT, 0);
	}
	hashkey = (uint64_t *)fh_desc->addr;
	len = (ushort *)((char *)hashkey + sizeof(uint64_t));
	if (flags & FH_FSAL_BIG_ENDIAN) {
#if (BYTE_ORDER != BIG_ENDIAN)
		*len = bswap_16(*len);
		*hashkey = bswap_64(*hashkey);
#endif
	} else {
#if (BYTE_ORDER == BIG_ENDIAN)
		*len = bswap_16(*len);
		*hashkey = bswap_64(*hashkey);
#endif
	}
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Allocate a state_t structure
 *
 * Note that this is not expected to fail since memory allocation is
 * expected to abort on failure.
 *
 * @param[in] exp_hdl               Export state_t will be associated with
 * @param[in] state_type            Type of state to allocate
 * @param[in] related_state         Related state if appropriate
 *
 * @returns a state structure.
 */

static struct state_t *s3_alloc_state(struct fsal_export *exp_hdl,
				      enum state_type state_type,
				      struct state_t *related_state)
{
	struct state_t *state;

	state = init_state(gsh_calloc(1, sizeof(struct state_t)
				      + sizeof(struct fsal_fd)),
			   exp_hdl, state_type, related_state);

	LogFullDebug(COMPONENT_FSAL, "%s: state %p",
		     __func__, state);

	return state;
}

/**
 * @brief Set operations for exports
 *
 * This function overrides operations that we've implemented, leaving
 * the rest for the default.
 *
 * @param[in,out] ops Operations vector
 */

void s3_export_ops_init(struct export_ops *ops)
{
	ops->release = s3_release_export;
	ops->lookup_path = s3_lookup_path;
	ops->wire_to_host = s3_wire_to_host;
	ops->create_handle = s3_create_handle;
	ops->get_fs_dynamic_info = s3_get_dynamic_info;
	ops->fs_supports = fs_supports;
	ops->fs_maxfilesize = fs_maxfilesize;
	ops->fs_maxread = fs_maxread;
	ops->fs_maxwrite = fs_maxwrite;
	ops->fs_maxlink = fs_maxlink;
	ops->fs_maxnamelen = fs_maxnamelen;
	ops->fs_maxpathlen = fs_maxpathlen;
	ops->fs_lease_time = fs_lease_time;
	ops->fs_acl_support = fs_acl_support;
	ops->fs_supported_attrs = fs_supported_attrs;
	ops->fs_umask = fs_umask;
	ops->fs_xattr_access_rights = fs_xattr_access_rights;
	ops->alloc_state = s3_alloc_state;
}

/**
 * @brief Create a new export under this FSAL
 *
 * This function creates a new export object for the RGW FSAL.
 *
 * @todo ACE: We do not handle re-exports of the same cluster in a
 * sane way.  Currently we create multiple handles and cache objects
 * pointing to the same one.  This is not necessarily wrong, but it is
 * inefficient.  It may also not be something we expect to use enough
 * to care about.
 *
 * @param[in]     module_in  The supplied module handle
 * @param[in]     path       The path to export
 * @param[in]     options    Export specific options for the FSAL
 * @param[in,out] list_entry Our entry in the export list
 * @param[in]     next_fsal  Next stacked FSAL
 * @param[out]    pub_export Newly created FSAL export object
 *
 * @return FSAL status.
 */

static struct config_item export_params[] = {
	CONF_ITEM_NOOP("name"),
	CONF_MAND_STR("host", 0, S3_MAX_HOSTNAME_SIZE, NULL,
		      s3_fsal_export, s3_host),
	CONF_MAND_STR("bucket_name", 0, S3_MAX_BUCKET_NAME_SIZE, NULL,
		      s3_fsal_export, s3_bucket),
	CONF_MAND_STR("access_key_id", S3_MIN_ACCESS_KEY_ID_SIZE, S3_MAX_ACCESS_KEY_ID_SIZE, NULL,
		      s3_fsal_export, s3_access_key),
	CONF_MAND_STR("secret_access_key", S3_MIN_SECRET_ACCESS_KEY_SIZE, S3_MAX_SECRET_ACCESS_KEY_SIZE, NULL,
		      s3_fsal_export, s3_secret_key),
	CONFIG_EOL
};

static struct config_block export_param_block = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.s3-export%d",
	.blk_desc.name = "FSAL",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = export_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

#ifdef OLD_STRUCT
static fsal_status_t create_export(struct fsal_module *module_in,
				   void *parse_node,
				   struct config_error_type *err_type,
				   const struct fsal_up_vector *up_ops)
{
	/* The status code to return */
	fsal_status_t status = { ERR_FSAL_NO_ERROR, 0 };
	/* The internal export object */
	struct s3_fsal_export *export = NULL;
	/* The 'private' root handle */
	struct s3_fsal_obj_handle *handle = NULL;
	/* Stat for root */
	struct stat st;
	/* Return code */
	int rc = 0;
	/* Return code from RGW calls */
	//int rgw_status; //AR: unused
	/* True if we have called fsal_export_init */
	bool initialized = false;

	/* once */
	/*AR: rgw_fsal_module has no member 'rgw'
	if (!RGWFSM.rgw) {
		PTHREAD_MUTEX_lock(&init_mtx);
		if (!RGWFSM.rgw) {
			char *conf_path = NULL;
			char *inst_name = NULL;
			char *cluster = NULL;

			int argc = 1;
			char *argv[5] = { "nfs-ganesha",
					  NULL, NULL, NULL, NULL };
			int clen;

			if (RGWFSM.conf_path) {
				clen = strlen(RGWFSM.conf_path) + 8;
				conf_path = (char *) gsh_malloc(clen);
				sprintf(conf_path, "--conf=%s",
					RGWFSM.conf_path);
				argv[argc] = conf_path;
				++argc;
			}

			if (RGWFSM.name) {
				clen = strlen(RGWFSM.name) + 8;
				inst_name = (char *) gsh_malloc(clen);
				sprintf(inst_name, "--name=%s", RGWFSM.name);
				argv[argc] = inst_name;
				++argc;
			}

			if (RGWFSM.cluster) {
				clen = strlen(RGWFSM.cluster) + 8;
				cluster = (char *) gsh_malloc(clen);
				sprintf(cluster, "--cluster=%s",
					RGWFSM.cluster);
				argv[argc] = cluster;
				++argc;
			}

			if (RGWFSM.init_args) {
				argv[argc] = RGWFSM.init_args;
				++argc;
			}

			rc = librgw_create(&RGWFSM.rgw, argc, argv);
			if (rc != 0) {
				LogCrit(COMPONENT_FSAL,
					"RGW module: librgw init failed (%d)",
					rc);
			}

			if (conf_path)
				gsh_free(conf_path);
			if (inst_name)
				gsh_free(inst_name);
			if (cluster)
				gsh_free(cluster);
		}
		PTHREAD_MUTEX_unlock(&init_mtx);
	}
    */

	if (rc != 0) {
		status.major = ERR_FSAL_BAD_INIT;
		goto error;
	}

	export = gsh_calloc(1, sizeof(struct s3_fsal_export));
	fsal_export_init(&export->export);
	export_ops_init(&export->export.exp_ops);

	/* get params for this export, if any */
	if (parse_node) {
		rc = load_config_from_node(parse_node,
					   &export_param_block,
					   export,
					   true,
					   err_type);

		if (rc != 0) {
			gsh_free(export);
			return fsalstat(ERR_FSAL_INVAL, 0);
		}
	}

	/* Fill the s3 bucket context */
	export->bucket_ctx.hostName = gsh_malloc(S3_MAX_HOSTNAME_SIZE);
	strncpy((char*)export->bucket_ctx.hostName, export->s3fs_host, S3_MAX_HOSTNAME_SIZE);
	export->bucket_ctx.bucketName = gsh_malloc(S3_MAX_BUCKET_NAME_SIZE);
	strncpy((char*)export->bucket_ctx.bucketName, export->s3fs_bucket, S3_MAX_BUCKET_NAME_SIZE);
	export->bucket_ctx.protocol = S3ProtocolHTTP;
	export->bucket_ctx.uriStyle = S3UriStylePath;
	export->bucket_ctx.accessKeyId = gsh_malloc(S3_MAX_KEY_SIZE);
	strncpy((char*)export->bucket_ctx.accessKeyId, export->s3fs_access_key, S3_MAX_KEY_SIZE);
	export->bucket_ctx.secretAccessKey = gsh_malloc(S3_MAX_KEY_SIZE);
	strncpy((char*)export->bucket_ctx.secretAccessKey, export->s3fs_secret_key, S3_MAX_KEY_SIZE);
	export->bucket_ctx.authRegion = NULL;

	/*AR: TODO: at this point we should have authenticated/authorized the user to
	* access this server/bucket with his/her credentials
	*/



	/**
	 * Lists keys within a bucket.
	 *
	 * @param bucketContext gives the bucket and associated parameters for this
	 *        request
	 * @param prefix if present and non-empty, gives a prefix for matching keys
	 * @param marker if present and non-empty, only keys occuring after this value
	 *        will be listed
	 * @param delimiter if present and non-empty, causes keys that contain the
	 *        same string between the prefix and the first occurrence of the
	 *        delimiter to be rolled up into a single result element
	 * @param maxkeys is the maximum number of keys to return
	 * @param requestContext if non-NULL, gives the S3RequestContext to add this
	 *        request to, and does not perform the request immediately.  If NULL,
	 *        performs the request immediately and synchronously.
	 * @param timeoutMs if not 0 contains total request timeout in milliseconds
	 * @param handler gives the callbacks to call as the request is processed and
	 *        completed
	 * @param callbackData will be passed in as the callbackData parameter to
	 *        all callbacks for this request
	 **/
//	S3Status s3st =	list_bucket(&export->bucket_ctx, NULL, NULL, NULL, 0, 1);
	S3Status s3st;
	s3st = list_bucket(&export->bucket_ctx, NULL, NULL, NULL, 0, 1);
//	if (s3st != S3StatusOK)
	initialized = (s3st == S3StatusOK);

#if 0 //AR:
#ifndef USE_FSAL_RGW_MOUNT2
	rgw_status = rgw_mount(RGWFSM.rgw,
			       export->rgw_user_id,
			       export->rgw_access_key_id,
			       export->rgw_secret_access_key,
			       &export->rgw_fs,
			       RGW_MOUNT_FLAG_NONE);
#else
	rgw_status = rgw_mount2(RGWFSM.rgw,
				export->rgw_user_id,
				export->rgw_access_key_id,
				export->rgw_secret_access_key,
				op_ctx->ctx_export->fullpath,
				&export->rgw_fs,
				RGW_MOUNT_FLAG_NONE);
#endif
	if (rgw_status != 0) {
		status.major = ERR_FSAL_SERVERFAULT;
		LogCrit(COMPONENT_FSAL,
			"Unable to mount RGW cluster for %s.",
			op_ctx->ctx_export->fullpath);
		if (rgw_status == -EINVAL) {
			LogCrit(COMPONENT_FSAL,
			"Authorization Failed for user %s ",
			export->rgw_user_id);
		}
		goto error;
	}
#endif




	/*AR: allocate and initialize s3fs_fs field of our export */
	export->s3fs_fs = gsh_calloc(1, sizeof(struct s3fs_fs));

	// construct 3fs_fs unique instance
	export->s3fs_fs->root_fh = gsh_calloc(1, sizeof(struct s3_file_handle));

	// create the root file handle
	export->s3fs_fs->root_fh->fh_type = S3_FS_TYPE_DIRECTORY;
	strcpy(export->s3fs_fs->root_fh->fh_hk.fullpath, "/");

	if (fsal_attach_export(module_in, &export->export.exports) != 0) {
		status.major = ERR_FSAL_SERVERFAULT;
		LogCrit(COMPONENT_FSAL,
			"Unable to attach export for %s.",
			op_ctx->ctx_export->fullpath);
		goto error;
	}

    /*AR: don't know for now
	if (rgw_register_invalidate(export->rgw_fs, rgw_fs_invalidate,
					up_ops->up_fsal_export,
					RGW_REG_INVALIDATE_FLAG_NONE) != 0) {
		LogCrit(COMPONENT_FSAL,
			"Unable to register invalidates for %s.",
			op_ctx->ctx_export->fullpath);
		goto error;
	}
    */

	export->export.fsal = module_in;

	LogDebug(COMPONENT_FSAL,
		 "S3 module export %s.",
		 op_ctx->ctx_export->fullpath);

#if 1 //AR: don't know for now so I set default posix attributes
/*
    struct attrlist attrs;
    attrs.valid_mask = ATTR_MODE;
    attrs.mode = 0755;
    attrs.filesize = 0;
*/
    /* AR: totally randomly invented :-p */
    dev_t posix_devid;
    posix_devid = 2054; //AR: random but should be consistent with others handles. (make a #define)
    st.st_mode = 0755;
    st.st_dev = posix_devid;


#else
	rc = rgw_getattr(export->rgw_fs, export->rgw_fs->root_fh, &st,
			RGW_GETATTR_FLAG_NONE);
	if (rc < 0)
		return rgw2fsal_error(rc);
#endif
	rc = construct_handle(export, export->s3fs_fs->root_fh, &st, &handle);
	if (rc < 0) {
		status = s32fsal_error(rc);
		goto error;
	}

	op_ctx->fsal_export = &export->export;

	export->root_handle = handle;
	export->export.up_ops = up_ops;

	return status;

 error:
	if (export) {
		gsh_free(export);
	}

	if (initialized)
		initialized = false;

	return status;
}
#else

fsal_status_t s3_create_export(struct fsal_module *module_in,
			       void *parse_node,
			       struct config_error_type *err_type,
			       const struct fsal_up_vector *up_ops)
{
	struct s3_fsal_export *myself;
	int retval = 0;
	int rc;
	pthread_rwlockattr_t attrs;

	myself = gsh_calloc(1, sizeof(struct s3_fsal_export));

	glist_init(&myself->mfe_objs);
	pthread_rwlockattr_init(&attrs);
#ifdef GLIBC
	pthread_rwlockattr_setkind_np(&attrs,
		PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
	PTHREAD_RWLOCK_init(&myself->mfe_exp_lock, &attrs);
	fsal_export_init(&myself->export);
	s3_export_ops_init(&myself->export.exp_ops);

	/* get params for this export, if any */
	if (parse_node) {
		rc = load_config_from_node(parse_node,
					   &export_param_block,
					   myself,
					   true,
					   err_type);

		if (rc != 0) {
			gsh_free(myself);
			return fsalstat(ERR_FSAL_INVAL, 0);
		}
	}

	/* Configure libs3 bucket context structure */
	myself->bucket_ctx.hostName = myself->s3_host;
	myself->bucket_ctx.bucketName = myself->s3_bucket;
	myself->bucket_ctx.protocol = S3ProtocolHTTP;
	myself->bucket_ctx.uriStyle = S3UriStylePath;
	myself->bucket_ctx.accessKeyId = myself->s3_access_key;
	myself->bucket_ctx.secretAccessKey = myself->s3_secret_key;
	myself->bucket_ctx.authRegion = NULL;

	/* Initialize libs3 */
	if ((s3st = S3_initialize("nfs-ganesha", S3_INIT_ALL, myself->s3_host)
				  != S3StatusOK)) {
		/*AR: TODO, better error handling */
		LogCrit(COMPONENT_FSAL,
			"S3 module can't initialize libS3.");
		gsh_free(myself);
		// AR: TODO, convert S3 errors to fsal stat errors
		return fsalstat(ERR_FSAL_BAD_INIT, 0);
	}

	/* AR: now we have 2 things to check before registering the nex export:
	 * 1. user credentials are valid
	 * 2. the specified bucket exists
	 */
	// TODO: le faire!!

	retval = fsal_attach_export(module_in, &myself->export.exports);

	if (retval != 0) {
		/* seriously bad */
		LogMajor(COMPONENT_FSAL,
			 "Could not attach export");
		free_export_ops(&myself->export);
		gsh_free(myself);	/* elvis has left the building */

		return fsalstat(posix2fsal_error(retval), retval);
	}

	myself->export.fsal = module_in;
	myself->export.up_ops = up_ops;

	/* Save the export path. */
//	myself->export_path = gsh_strdup(op_ctx->ctx_export->fullpath);
	op_ctx->fsal_export = &myself->export;

	/* Insert into exports list */
	glist_add_tail(&S3.s3_exports, &myself->export_entry);

//	LogDebug(COMPONENT_FSAL,
//		 "Created exp %p - %s",
//		 myself, myself->export_path);
	LogDebug(COMPONENT_FSAL,
		 "Created exp %p",
		 myself);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}
#endif
