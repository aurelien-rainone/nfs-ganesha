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
 * @file   internal.h
 * @brief Internal declarations for the S3 FSAL
 *
 * This file includes declarations of data types, functions,
 * variables, and constants for the S3 FSAL.
 */

#ifndef FSAL_S3_INTERNAL_INTERNAL
#define FSAL_S3_INTERNAL_INTERNAL

#include <stdbool.h>
#include <dirent.h> /* NAME_MAX */

#include "fsal.h"
#include "fsal_types.h"
#include "fsal_api.h"
#include "fsal_convert.h"
#include "sal_data.h"
#include "city.h" /* CityHash128 */

#include "avltree.h"
#include "gsh_list.h"


#include <libs3.h>


//AR: TODO: directly take from libs3.h
#define MAXUIDLEN 32
#define MAXKEYLEN 20
#define MAXSECRETLEN 40

/**
 * S3 internal export object
 */

struct s3_fsal_export {
	struct fsal_export export;		/*< The public export object */
	struct s3fs_fs *s3fs_fs;		/*< "Opaque" fs handle */
	struct s3_fsal_obj_handle *root_handle;	/*< root handle */
	char *s3fs_host;			/*< s3 host */
	char *s3fs_bucket;			/*< s3 bucket name to be mounted */
	char *s3fs_access_key;			/*< s3 access key id */
	char *s3fs_secret_key;			/*< s3 secret key */
	S3BucketContext bucket_ctx;
	struct glist_head export_entry;		/*< Entry into list of exports */
	pthread_rwlock_t mfe_exp_lock;		/*< Lock protecting mfe_objs */
	struct glist_head mfe_objs;		/*< List of all the objects in this export */
};

fsal_status_t s3_lookup_path(struct fsal_export *exp_hdl,
				const char *path,
				struct fsal_obj_handle **handle,
				struct attrlist *attrs_out);

fsal_status_t s3_create_handle(struct fsal_export *exp_hdl,
				  struct gsh_buffdesc *hdl_desc,
				  struct fsal_obj_handle **handle,
				  struct attrlist *attrs_out);


/**
 * AR: file system handle: its main feature is to own the root file handle
 */

struct s3fs_fs{
    struct s3_file_handle* root_fh;	/*< Root file handle */
};

/**
 * The S3 FSAL internal handle
 */

#define V4_FH_OPAQUE_SIZE 58 /* Size of state_obj digest */

struct s3_fsal_obj_handle {
	struct fsal_obj_handle obj_handle;
	struct attrlist attrs;
	uint64_t inode;
	char handle[V4_FH_OPAQUE_SIZE];
	union {
		struct {
			struct s3_fsal_obj_handle *parent;
			struct avltree avl_name;
			struct avltree avl_index;
			uint32_t numkids;
			uint32_t next_i; /* next child index */
		} mh_dir;
		struct {
			struct fsal_share share;
			struct fsal_fd fd;
		} mh_file;
		struct {
			object_file_type_t nodetype;
			fsal_dev_t dev;
		} mh_node;
		struct {
			char *link_contents;
		} mh_symlink;
	};
	struct glist_head dirents; /* List of dirents pointing to obj */
	struct glist_head mfo_exp_entry; /**< Link into mfs_objs */
	struct s3_fsal_export *mfo_exp; /**< Export owning object */
	char *m_name;	/**< Base name of obj, for debugging */
//	uint32_t datasize;
	bool is_export;
//	char data[0]; /* Allocated data */
};

/**
 * @brief Dirent for FSAL_S3
 */
struct s3_dirent {
	struct s3_fsal_obj_handle *hdl; /**< Handle dirent points to */
	struct s3_fsal_obj_handle *dir; /**< Dir containing dirent */
	const char *d_name;		 /**< Name of dirent */
	uint32_t d_index;		 /**< index in dir */
	struct avltree_node avl_n;	 /**< Entry in dir's avl_name tree */
	struct avltree_node avl_i;	 /**< Entry in dir's avl_index tree */
	struct glist_head dlist;	 /**< Entry in hdl's dirents list */
};

void s3_handle_ops_init(struct fsal_obj_ops *ops);

/* Internal S3 method linkage to export object
*/

fsal_status_t s3_create_export(struct fsal_module *fsal_hdl,
			       void *parse_node,
			       struct config_error_type *err_type,
			       const struct fsal_up_vector *up_ops);

#define s3_free_handle(h) _s3_free_handle(h, __func__, __LINE__)
/**
 * @brief Free a S3 handle
 *
 * @note mfe_exp_lock MUST be held for write
 * @param[in] hdl	Handle to free
 */
static inline void _s3_free_handle(struct s3_fsal_obj_handle *hdl,
				    const char *func, int line)
{
#ifdef USE_LTTNG
	tracepoint(fsalmem, mem_free, func, line, hdl, hdl->m_name);
#endif

	glist_del(&hdl->mfo_exp_entry);
	hdl->mfo_exp = NULL;

	if (hdl->m_name != NULL) {
		gsh_free(hdl->m_name);
		hdl->m_name = NULL;
	}

	gsh_free(hdl);
}


/* Prototypes */
void s3_clean_export(struct s3_fsal_obj_handle *root);
void s3_clean_all_dirents(struct s3_fsal_obj_handle *parent);

/**
 * @brief FSAL Module wrapper for S3
 */
struct s3_fsal_module {
	/** Module we're wrapping */
	struct fsal_module fsal;
	/** Our FS INFO */
	struct fsal_staticfsinfo_t fs_info;
	/** List of S3 exports. TODO Locking when we care */
	struct glist_head s3_exports;
	/** Config - size of data in inode */
	uint32_t inode_size;
	/** Config - Interval for UP call thread */
	uint32_t up_interval;
	/** Next unused inode */
	uint64_t next_inode;
};


/* UP testing */
fsal_status_t s3_up_pkginit(void);
fsal_status_t s3_up_pkgshutdown(void);

extern struct s3_fsal_module S3;

#endif				/* !FSAL_S3_INTERNAL_INTERNAL */
