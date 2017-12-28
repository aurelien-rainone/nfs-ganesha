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
 * @file   FSAL_S3/up.c
 * @author Aurélien Rainone <a.rainone@cynnyspace.com>
 * @date   Fri Dec 28 16:45:01 2017
 *
 * @brief Upcalls
 *
 * Use new generic invalidate hook to drive upcalls.
 */

#include <fcntl.h>
#include <stdlib.h>
#include "fsal.h"
#include "fsal_convert.h"
#include "internal.h"

static struct fridgethr *s3_up_fridge;

/**
 * @brief Invalidate an object
 *
 * This function sends an invalidate for an object.  The object itself is not
 * really deleted, since there's no way to get it back, but it should allow
 * testing of the invalidate UP call.
 *
 * @param[in] mfe	S3 export owning handle
 * @param[in] hdl	Handle to invalidate
 */
static void
s3_invalidate(struct s3_fsal_export *mfe, struct s3_fsal_obj_handle *hdl)
{
	const struct fsal_up_vector *up_ops = mfe->export.up_ops;
	fsal_status_t status;
	struct gsh_buffdesc fh_desc;

	LogFullDebug(COMPONENT_FSAL_UP, "invalidating %s", hdl->m_name);

	hdl->obj_handle.obj_ops.handle_to_key(&hdl->obj_handle, &fh_desc);

	status = up_ops->invalidate(up_ops, &fh_desc, FSAL_UP_INVALIDATE_CACHE);
	if (FSAL_IS_ERROR(status)) {
		LogMajor(COMPONENT_FSAL_UP, "error invalidating %s: %s",
			 hdl->m_name, fsal_err_txt(status));
	}
}

/**
 * @brief Invalidate and close an object
 *
 * This function sends an invalidate_close for an object.  The object itself is
 * not really deleted, since there's no way to get it back, but it should allow
 * testing of the invalidate_close UP call.
 *
 * @param[in] mfe	S3 export owning handle
 * @param[in] hdl	Handle to invalidate
 */
static void
s3_invalidate_close(struct s3_fsal_export *mfe,
		     struct s3_fsal_obj_handle *hdl)
{
	const struct fsal_up_vector *up_ops = mfe->export.up_ops;
	fsal_status_t status;
	struct gsh_buffdesc fh_desc;

	LogFullDebug(COMPONENT_FSAL_UP, "invalidate_closing %s", hdl->m_name);

	hdl->obj_handle.obj_ops.handle_to_key(&hdl->obj_handle, &fh_desc);

	status = up_ops->invalidate_close(up_ops, &fh_desc,
					  FSAL_UP_INVALIDATE_CACHE);
	if (FSAL_IS_ERROR(status)) {
		LogMajor(COMPONENT_FSAL_UP, "error invalidate_closing %s: %s",
			 hdl->m_name, fsal_err_txt(status));
	}
}

/**
 * @brief Update an object
 *
 * This function sends an update for an object.  In this case, we update some of
 * the times, just so something changed.
 *
 * @param[in] mfe	S3 export owning handle
 * @param[in] hdl	Handle to update
 */
static void
s3_update(struct s3_fsal_export *mfe, struct s3_fsal_obj_handle *hdl)
{
	const struct fsal_up_vector *up_ops = mfe->export.up_ops;
	fsal_status_t status;
	struct gsh_buffdesc fh_desc;
	struct attrlist attrs;

	LogFullDebug(COMPONENT_FSAL_UP, "updating %s", hdl->m_name);

	hdl->obj_handle.obj_ops.handle_to_key(&hdl->obj_handle, &fh_desc);

	fsal_prepare_attrs(&attrs, 0);
	/* Set CTIME */
	now(&hdl->attrs.ctime);
	attrs.ctime = hdl->attrs.ctime; /* struct copy */
	FSAL_SET_MASK(attrs.valid_mask, ATTR_CTIME);

	/* Set change and chgtime */
	hdl->attrs.chgtime = attrs.ctime; /* struct copy */
	attrs.chgtime = hdl->attrs.chgtime; /* struct copy */
	FSAL_SET_MASK(attrs.valid_mask, ATTR_CHGTIME);
	hdl->attrs.change++;
	attrs.change = hdl->attrs.change;
	FSAL_SET_MASK(attrs.valid_mask, ATTR_CHANGE);

	status = up_ops->update(up_ops, &fh_desc, &attrs, fsal_up_update_null);
	if (FSAL_IS_ERROR(status)) {
		LogMajor(COMPONENT_FSAL_UP, "error updating %s: %s",
			 hdl->m_name, fsal_err_txt(status));
	}
}

/**
 * @brief Select a random obj from an export
 *
 * @param[in] mfe	Export to select from
 * @return Obj on success, NULL on failure
 */
struct s3_fsal_obj_handle *
s3_rand_obj(struct s3_fsal_export *mfe)
{
	struct s3_fsal_obj_handle *res = NULL;
	struct glist_head *glist, *glistn;
	uint32_t n = 2;

	if (glist_empty(&mfe->mfe_objs))
		return NULL;

	PTHREAD_RWLOCK_rdlock(&mfe->mfe_exp_lock);
	glist_for_each_safe(glist, glistn, &mfe->mfe_objs) {
		if (res == NULL) {
			/* Grab first entry */
			res = glist_entry(glist, struct s3_fsal_obj_handle,
					  mfo_exp_entry);
			continue;
		}

		if (rand() % n == 0) {
			/* Replace with current */
			res = glist_entry(glist, struct s3_fsal_obj_handle,
					  mfo_exp_entry);
			break;
		}
		n++;
	}
	PTHREAD_RWLOCK_unlock(&mfe->mfe_exp_lock);

	return res;
}

/**
 * @brief Run an iteration of the UP call thread
 *
 * Each iteration exercises various UP calls.
 *
 * - Pick a random obj in each export, and invalidate it
 *
 * @param[in] ctx	Thread context
 * @return Return description
 */
static void
s3_up_run(struct fridgethr_context *ctx)
{
	struct glist_head *glist, *glistn;

	glist_for_each_safe(glist, glistn, &S3.s3_exports) {
		struct s3_fsal_export *mfe;
		struct s3_fsal_obj_handle *hdl;

		mfe = glist_entry(glist, struct s3_fsal_export, export_entry);

		/* Update a handle */
		hdl = s3_rand_obj(mfe);
		if (hdl)
			s3_update(mfe, hdl);

		/* Invalidate a handle */
		hdl = s3_rand_obj(mfe);
		if (hdl)
			s3_invalidate(mfe, hdl);

		/* Invalidate and close a handle */
		hdl = s3_rand_obj(mfe);
		if (hdl)
			s3_invalidate_close(mfe, hdl);
	}
}

/**
 * Initialize subsystem
 */
fsal_status_t
s3_up_pkginit(void)
{
	/* Return code from system calls */
	int code = 0;
	struct fridgethr_params frp;

	if (S3.up_interval == 0) {
		/* Don't run up-thread */
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	if (s3_up_fridge) {
		/* Already initialized */
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	memset(&frp, 0, sizeof(struct fridgethr_params));
	frp.thr_max = 1;
	frp.thr_min = 1;
	frp.thread_delay = S3.up_interval;
	frp.flavor = fridgethr_flavor_looper;

	/* spawn S3_UP background thread */
	code = fridgethr_init(&s3_up_fridge, "S3_UP_fridge", &frp);
	if (code != 0) {
		LogMajor(COMPONENT_FSAL_UP,
			 "Unable to initialize S3_UP fridge, error code %d.",
			 code);
		return posix2fsal_status(code);
	}

	code = fridgethr_submit(s3_up_fridge, s3_up_run, NULL);
	if (code != 0) {
		LogMajor(COMPONENT_FSAL_UP,
			 "Unable to start S3_UP thread, error code %d.", code);
		return fsalstat(posix2fsal_error(code), code);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * Shutdown subsystem
 *
 * @return FSAL status
 */
fsal_status_t
s3_up_pkgshutdown(void)
{
	if (!s3_up_fridge) {
		/* Interval wasn't configured */
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	int rc = fridgethr_sync_command(s3_up_fridge,
					fridgethr_comm_stop,
					120);

	if (rc == ETIMEDOUT) {
		LogMajor(COMPONENT_FSAL_UP,
			 "Shutdown timed out, cancelling threads.");
		fridgethr_cancel(s3_up_fridge);
	} else if (rc != 0) {
		LogMajor(COMPONENT_FSAL_UP,
			 "Failed shutting down S3_UP thread: %d", rc);
	}

	fridgethr_destroy(s3_up_fridge);
	s3_up_fridge = NULL;
	return fsalstat(posix2fsal_error(rc), rc);
}
