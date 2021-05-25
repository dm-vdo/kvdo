/*
 * Copyright Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vio.c#42 $
 */

#include "vio.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "dataVIO.h"
#include "vdoInternal.h"

#include <linux/ratelimit.h>

/**********************************************************************/
int create_metadata_vio(struct vdo *vdo,
			enum vio_type vio_type,
			enum vio_priority priority,
			void *parent,
			char *data,
			struct vio **vio_ptr)
{
	struct vio *vio;
	struct bio *bio;
	int result;

	// If struct vio grows past 256 bytes, we'll lose benefits of
	// VDOSTORY-176.
	STATIC_ASSERT(sizeof(struct vio) <= 256);

	result = ASSERT(is_vdo_metadata_vio_type(vio_type),
			"%d is a metadata type",
			vio_type);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Metadata vios should use direct allocation and not use the buffer
	// pool, which is reserved for submissions from the linux block layer.
	result = ALLOCATE(1, struct vio, __func__, &vio);
	if (result != VDO_SUCCESS) {
		uds_log_error("metadata vio allocation failure %d", result);
		return result;
	}

	result = vdo_create_bio(&bio);
	if (result != VDO_SUCCESS) {
		FREE(vio);
		return result;
	}

	initialize_vio(vio,
		       bio,
		       vio_type,
		       priority,
		       parent,
		       vdo,
		       data);
	*vio_ptr  = vio;
	return VDO_SUCCESS;

}

/**********************************************************************/
void free_vio(struct vio **vio_ptr)
{
	struct vio *vio = *vio_ptr;
	if (vio == NULL) {
		return;
	}

	destroy_vio(vio_ptr);
}

/**********************************************************************/
void initialize_vio(struct vio *vio,
		    struct bio *bio,
		    enum vio_type vio_type,
		    enum vio_priority priority,
		    struct vdo_completion *parent,
		    struct vdo *vdo,
		    char *data)
{
	struct vdo_completion *completion = vio_as_completion(vio);

	vio->bio = bio;
	vio->vdo = vdo;
	vio->type = vio_type;
	vio->priority = priority;
	vio->data = data;

	initialize_vdo_completion(completion, vdo, VIO_COMPLETION);
	completion->parent = parent;
}

/**********************************************************************/
void vio_done_callback(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	completion->callback = vio->callback;
	completion->error_handler = vio->error_handler;
	complete_vdo_completion(completion);
}

/**********************************************************************/
void get_vio_operation_description(const struct vio *vio, char *buffer)
{
	int buffer_remaining = VDO_VIO_OPERATION_DESCRIPTION_MAX_LENGTH;

	static const char *operations[] = {
		[VIO_UNSPECIFIED_OPERATION] = "empty",
		[VIO_READ]		    = "read",
		[VIO_WRITE]		    = "write",
		[VIO_READ_MODIFY_WRITE]	    = "read-modify-write",
	};
	int written = snprintf(buffer, buffer_remaining, "%s",
		operations[vio->operation & VIO_READ_WRITE_MASK]);
	if ((written < 0) || (buffer_remaining < written)) {
		// Should never happen, but if it does, we've done as much
		// description as possible.
		return;
	}

	buffer += written;
	buffer_remaining -= written;

	if (vio->operation & VIO_FLUSH_BEFORE) {
		written = snprintf(buffer, buffer_remaining, "+preflush");
	}

	if ((written < 0) || (buffer_remaining < written)) {
		// Should never happen, but if it does, we've done as much
		// description as possible.
		return;
	}

	buffer += written;
	buffer_remaining -= written;

	if (vio->operation & VIO_FLUSH_AFTER) {
		snprintf(buffer, buffer_remaining, "+postflush");
	}

	STATIC_ASSERT(sizeof("write+preflush+postflush") <=
		      VDO_VIO_OPERATION_DESCRIPTION_MAX_LENGTH);
}

/**********************************************************************/
void update_vio_error_stats(struct vio *vio, const char *format, ...)
{
	static DEFINE_RATELIMIT_STATE(error_limiter,
				      DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	va_list args;
	int priority;

	int result = vio_as_completion(vio)->result;
	switch (result) {
	case VDO_READ_ONLY:
		atomic64_inc(&vio->vdo->error_stats.read_only_error_count);
		return;

	case VDO_NO_SPACE:
		atomic64_inc(&vio->vdo->error_stats.no_space_error_count);
		priority = LOG_DEBUG;
		break;

	default:
		priority = LOG_ERR;
	}

	if (!__ratelimit(&error_limiter)) {
		return;
	}

	va_start(args, format);
	vlog_strerror(priority, result, format, args);
	va_end(args);
}

/**
 * Handle an error from a metadata I/O.
 *
 * @param completion  The vio
 **/
static void handle_metadata_io_error(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	char vio_operation[VDO_VIO_OPERATION_DESCRIPTION_MAX_LENGTH];
	get_vio_operation_description(vio, vio_operation);
	update_vio_error_stats(vio,
			       "Completing %s vio of type %u for physical block %llu with error",
			       vio_operation,
			       vio->type,
			       vio->physical);
	vio_done_callback(completion);
}

/**********************************************************************/
void launch_metadata_vio(struct vio *vio,
			 physical_block_number_t physical,
			 vdo_action *callback,
			 vdo_action *error_handler,
			 enum vio_operation operation)
{
	struct vdo_completion *completion = vio_as_completion(vio);

	vio->operation = operation;
	vio->physical = physical;
	vio->callback = callback;
	vio->error_handler = error_handler;

	reset_vdo_completion(completion);
	completion->callback = vio_done_callback;
	completion->error_handler = handle_metadata_io_error;

	submit_metadata_vio(vio);
}
