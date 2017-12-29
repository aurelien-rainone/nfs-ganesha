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

/* main.c
 * Module core functions
 */

#include <stdlib.h>
#include <assert.h>
#include "gsh_list.h"
#include "fsal.h"
#include "fsal_types.h"
#include "FSAL/fsal_init.h"
#include "FSAL/fsal_commonlib.h"
#include "fsal_api.h"
#include "internal.h"
#include "abstract_mem.h"
#include "nfs_exports.h"
#include "export_mgr.h"

static const char *module_name = "S3";

/* defined the set of attributes supported with POSIX */
#define S3_SUPPORTED_ATTRIBUTES (ATTRS_POSIX)

/* filesystem info for S3 */
static struct fsal_staticfsinfo_t default_s3_info = {
	.maxfilesize = UINT64_MAX,
	.maxlink = _POSIX_LINK_MAX,
	.maxnamelen = 1024,
	.maxpathlen = 1024,
	.no_trunc = true,
	.chown_restricted = true,
	.case_insensitive = false,
	.case_preserving = true,
	.link_support = false,
	.symlink_support = false,
	.lock_support = false,
	.lock_support_async_block = false,
	.named_attr = false, /* XXX */
	.unique_handles = true,
	.lease_time = {10, 0},
	.acl_support = false,
	.cansettime = true,
	.homogenous = true,
	.supported_attrs = S3_SUPPORTED_ATTRIBUTES,
	.maxread = FSAL_MAXIOSIZE,
	.maxwrite = FSAL_MAXIOSIZE,
	.umask = 0,
	.rename_changes_key = true,
	.auth_exportpath_xdev = false,
	.xattr_access_rights = 0400,	/* root=RW, owner=R */
	.link_supports_permission_checks = false,
};

/*AR: check if 2 level of configuration are needed for S3
 */
static struct config_item s3_items[] = {
	CONFIG_EOL
};

struct config_block s3_block = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.s3",
	.blk_desc.name = "S3",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = s3_items,
	.blk_desc.u.blk.commit = noop_conf_commit
};

/* private helper for export object
 */

struct fsal_staticfsinfo_t *s3_staticinfo(struct fsal_module *hdl)
{
	struct s3_fsal_module *myself;

	myself = container_of(hdl, struct s3_fsal_module, fsal);
	return &myself->fs_info;
}

/* Initialize s3 fs info */

static fsal_status_t s3_init_config(struct fsal_module *module_in,
				    config_file_t config_struct,
				    struct config_error_type *err_type)
{
	struct s3_fsal_module *s3_me =
	    container_of(module_in, struct s3_fsal_module, fsal);
	fsal_status_t status = {0, 0};

	LogDebug(COMPONENT_FSAL, "S3 module setup.");

	/* get a copy of the defaults */
	s3_me->fs_info = default_s3_info;

	/* if we have fsal specific params, do them here
	 * fsal_hdl->name is used to find the block containing the
	 * params.
	 */
	(void) load_config_from_parse(config_struct,
				      &s3_block,
				      s3_me,
				      true,
				      err_type);
	if (!config_error_is_harmless(err_type))
		return fsalstat(ERR_FSAL_INVAL, 0);

	/* Initialize UP calls */
	status = s3_up_pkginit();
	if (FSAL_IS_ERROR(status)) {
		LogMajor(COMPONENT_FSAL,
			 "Failed to initialize FSAL_S3 UP package %s",
			 fsal_err_txt(status));
		return status;
	}

	display_fsinfo(&s3_me->fs_info);
	LogFullDebug(COMPONENT_FSAL,
		     "Supported attributes constant = 0x%" PRIx64,
		     (uint64_t) S3_SUPPORTED_ATTRIBUTES);
	LogFullDebug(COMPONENT_FSAL,
		     "Supported attributes default = 0x%" PRIx64,
		     default_s3_info.supported_attrs);
	LogDebug(COMPONENT_FSAL,
		 "FSAL INIT: Supported attributes mask = 0x%" PRIx64,
		 s3_me->fs_info.supported_attrs);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* Module initialization.
 * Called by dlopen() to register the module
 * keep a private pointer to me in myself
 */

/* linkage to the exports and handle ops initializers
 */

/**
 * @brief Initialize and register the FSAL
 *
 * This function initializes the FSAL module handle and libs3.  It exists solely
 * to produce a properly constructed FSAL module handle.
 */

MODULE_INIT void init(void)
{
	struct fsal_module *myself = &S3.fsal;
	S3Status s3st;

	LogDebug(COMPONENT_FSAL,
		 "S3 module registering.");

	/* register_fsal seems to expect zeroed memory. */
	memset(myself, 0, sizeof(*myself));

	if (register_fsal(myself, module_name, FSAL_MAJOR_VERSION,
			  FSAL_MINOR_VERSION, FSAL_ID_RGW) != 0) {
		/* The register_fsal function prints its own log
		   message if it fails */
		LogCrit(COMPONENT_FSAL,
			"S3 module failed to register.");
	}

	/* Set up module operations */
	myself->m_ops.create_export = s3_create_export;
	myself->m_ops.init_config = s3_init_config;
	glist_init(&S3.s3_exports);
	S3.next_inode = 0xc0ffee;

	/* Initialize libs3 */
	if ((s3st = S3_initialize("nfs-ganesha", S3_INIT_ALL, NULL)
				  != S3StatusOK)) {
		/*AR: TODO, better error handling */
		LogCrit(COMPONENT_FSAL,
			"S3 module can't initialize libS3.");
	}
}

/**
 * @brief Release FSAL resources
 *
 * This function unregisters the FSAL and frees its module handle.  The
 * FSAL also has an open instance of the libs3 library, so we also need to
 * release that.
 */

MODULE_FINI void finish(void)
{
	int retval;

	LogDebug(COMPONENT_FSAL,
		 "S3 module finishing.");

	/* Shutdown UP calls */
	s3_up_pkgshutdown();

	retval = unregister_fsal(&S3.fsal);
	if (retval != 0) {
		LogCrit(COMPONENT_FSAL,
			"Unable to unload S3 FSAL.  Dying with extreme prejudice.");
		abort();
	}
	/* release the library */
	S3_deinitialize();
}
