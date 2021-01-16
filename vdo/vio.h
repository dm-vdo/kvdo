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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vio.h#36 $
 */

#ifndef VIO_H
#define VIO_H

#include <stdarg.h>

#include "kernelLayer.h"

#include "completion.h"
#include "types.h"
#include "vdo.h"

/**
 * A representation of a single block which may be passed between the VDO base
 * and the physical layer.
 **/
struct vio {
	/* The completion for this vio */
	struct vdo_completion completion;

	/* The functions to call when this vio's operation is complete */
	vdo_action *callback;
	vdo_action *error_handler;

	/* The vdo handling this vio */
	struct vdo *vdo;

	/* The address on the underlying device of the block to be read/written
	 */
	physical_block_number_t physical;

	/* The type of request this vio is servicing */
	enum vio_operation operation;

	/* The queueing priority of the vio operation */
	enum vio_priority priority;

	/* The vio type is used for statistics and instrumentation. */
	enum vio_type type;

	/* The data being read or written. */
	char *data;

	/* The VDO-owned bio to use for all IO for this vio */
	struct bio *bio;
	/**
	 * A list of enqueued bios with consecutive block numbers, stored by
	 * enqueueBioMap under the first-enqueued vio. The other vios are
	 * found via their bio entries in this list, and are not added to
	 * the work queue as separate work items.
	 **/
	struct bio_list bios_merged;
	/** A slot for an arbitrary bit of data, for use by systemtap. */
	long debug_slot;
};

/**
 * Convert a generic vdo_completion to a vio.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a vio
 **/
static inline struct vio *as_vio(struct vdo_completion *completion)
{
	assert_completion_type(completion->type, VIO_COMPLETION);
	return container_of(completion, struct vio, completion);
}

/**
 * Returns a pointer to the vio wrapping a work item
 *
 * @param item  the work item
 *
 * @return the vio
 **/
static inline struct vio * __must_check
work_item_as_vio(struct vdo_work_item *item)
{
	return as_vio(container_of(item, struct vdo_completion, work_item));
}

/**
 * Convert a vio to a generic completion.
 *
 * @param vio The vio to convert
 *
 * @return The vio as a completion
 **/
static inline struct vdo_completion *vio_as_completion(struct vio *vio)
{
	return &vio->completion;
}

/**
 * Extracts the work item from a vio.
 *
 * @param vio  the vio
 *
 * @return the vio's work item
 **/
static inline struct vdo_work_item *work_item_from_vio(struct vio *vio)
{
	return &vio_as_completion(vio)->work_item;
}

/**
 * Create a vio. Defined per-layer.
 *
 * @param [in]  layer      The physical layer
 * @param [in]  vio_type   The type of vio to create
 * @param [in]  priority   The relative priority to assign to the vio
 * @param [in]  parent     The parent of the vio
 * @param [in]  data       The buffer
 * @param [out] vio_ptr    A pointer to hold the new vio
 *
 * @return VDO_SUCCESS or an error
 **/
int kvdo_create_metadata_vio(PhysicalLayer *layer,
			     enum vio_type vio_type,
			     enum vio_priority priority,
			     void *parent,
			     char *data,
			     struct vio **vio_ptr);

/**
 * Destroy a vio. The pointer to the vio will be nulled out.
 *
 * @param vio_ptr  A pointer to the vio to destroy
 **/
void free_vio(struct vio **vio_ptr);

/**
 * Initialize a vio.
 *
 * @param vio       The vio to initialize
 * @param vio_type  The vio type
 * @param priority  The relative priority of the vio
 * @param parent    The parent (the extent completion) to assign to the vio
 *                  completion
 * @param vdo       The vdo for this vio
 * @param layer     The layer for this vio
 **/
void initialize_vio(struct vio *vio,
		    enum vio_type vio_type,
		    enum vio_priority priority,
		    struct vdo_completion *parent,
		    struct vdo *vdo,
		    PhysicalLayer *layer);

/**
 * The very last step in processing a vio. Set the vio's completion's callback
 * and error handler from the fields set in the vio itself on launch and then
 * actually complete the vio's completion.
 *
 * @param completion  The vio
 **/
void vio_done_callback(struct vdo_completion *completion);

/**
 * Get the description of a vio's operation.
 *
 * The output buffer must have size VIO_OPERATION_DESCRIPTION_MAX_LENGTH.
 *
 * @param vio     The vio
 * @param buffer  The buffer to populate with the vio operation name.
 *
 * @return The name of the vio's operation (read, write, empty,
 *	   read-modify-write, possibly with additional preflush
 *	   or postflush)
 **/
void get_vio_operation_description(const struct vio *vio, char *buffer);

/**
 * Update per-vio error stats and log the error.
 *
 * @param vio     The vio which got an error
 * @param format  The format of the message to log (a printf style format)
 **/
void update_vio_error_stats(struct vio *vio, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/**
 * Check whether a vio is servicing an external data request.
 *
 * @param vio  The vio to check
 **/
static inline bool is_data_vio(struct vio *vio)
{
	return is_data_vio_type(vio->type);
}

/**
 * Check whether a vio is for compressed block writes
 *
 * @param vio  The vio to check
 **/
static inline bool is_compressed_write_vio(struct vio *vio)
{
	return is_compressed_write_vio_type(vio->type);
}

/**
 * Check whether a vio is for metadata
 *
 * @param vio  The vio to check
 **/
static inline bool is_metadata_vio(struct vio *vio)
{
	return is_metadata_vio_type(vio->type);
}

/**
 * Check whether a vio is a read.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio is a read
 **/
static inline bool is_read_vio(const struct vio *vio)
{
	return ((vio->operation & VIO_READ_WRITE_MASK) == VIO_READ);
}

/**
 * Check whether a vio is a read-modify-write.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio is a read-modify-write
 **/
static inline bool is_read_modify_write_vio(const struct vio *vio)
{
	return ((vio->operation & VIO_READ_WRITE_MASK) ==
		VIO_READ_MODIFY_WRITE);
}

/**
 * Check whether a vio is a empty flush.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio is a pure, empty flush
 **/
static inline bool is_empty_flush_vio(const struct vio *vio)
{
	return (vio->operation == VIO_FLUSH_BEFORE);
}

/**
 * Check whether a vio is a write.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio is a write
 **/
static inline bool is_write_vio(const struct vio *vio)
{
	return ((vio->operation & VIO_READ_WRITE_MASK) == VIO_WRITE);
}

/**
 * Check whether a vio requires a flush before doing its I/O.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio requires a flush before
 **/
static inline bool vio_requires_flush_before(const struct vio *vio)
{
	return ((vio->operation & VIO_FLUSH_BEFORE) == VIO_FLUSH_BEFORE);
}

/**
 * Check whether a vio requires a flush after doing its I/O.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio requires a flush after
 **/
static inline bool vio_requires_flush_after(const struct vio *vio)
{
	return ((vio->operation & VIO_FLUSH_AFTER) == VIO_FLUSH_AFTER);
}

/**
 * Launch a metadata vio.
 *
 * @param vio            The vio to launch
 * @param physical       The physical block number to read or write
 * @param callback       The function to call when the vio completes its I/O
 * @param error_handler  The handler for write errors
 * @param operation      The operation to perform (read or write)
 **/
void launch_metadata_vio(struct vio *vio,
			 physical_block_number_t physical,
			 vdo_action *callback,
			 vdo_action *error_handler,
			 enum vio_operation operation);

/**
 * Launch a metadata read vio.
 *
 * @param vio            The vio to launch
 * @param physical       The physical block number to read
 * @param callback       The function to call when the vio completes its read
 * @param error_handler  The handler for write errors
 **/
static inline void launch_read_metadata_vio(struct vio *vio,
					    physical_block_number_t physical,
					    vdo_action *callback,
					    vdo_action *error_handler)
{
	launch_metadata_vio(vio, physical, callback, error_handler, VIO_READ);
}

/**
 * Launch a metadata write vio.
 *
 * @param vio            The vio to launch
 * @param physical       The physical block number to write
 * @param callback       The function to call when the vio completes its write
 * @param error_handler  The handler for write errors
 **/
static inline void launch_write_metadata_vio(struct vio *vio,
					     physical_block_number_t physical,
					     vdo_action *callback,
					     vdo_action *error_handler)
{
	launch_metadata_vio(vio, physical, callback, error_handler, VIO_WRITE);
}

/**
 * Launch a metadata write vio optionally flushing the layer before and/or
 * after the write operation.
 *
 * @param vio           The vio to launch
 * @param physical      The physical block number to write
 * @param callback      The function to call when the vio completes its
 *                      operation
 * @param error_handler The handler for flush or write errors
 * @param flush_before  Whether or not to flush before writing
 * @param flush_after   Whether or not to flush after writing
 **/
static inline void
launch_write_metadata_vio_with_flush(struct vio *vio,
				     physical_block_number_t physical,
				     vdo_action *callback,
				     vdo_action *error_handler,
				     bool flush_before,
				     bool flush_after)
{
	launch_metadata_vio(vio,
			    physical,
			    callback,
			    error_handler,
			    (VIO_WRITE | (flush_before ? VIO_FLUSH_BEFORE : 0) |
			     (flush_after ? VIO_FLUSH_AFTER : 0)));
}

/**
 * Issue a flush to the layer. Currently expected to be used only in
 * async mode.
 *
 * @param vio            The vio to notify when the flush is complete
 * @param callback       The function to call when the flush is complete
 * @param error_handler  The handler for flush errors
 **/
static inline void launch_flush(struct vio *vio,
				vdo_action *callback,
				vdo_action *error_handler)
{
	launch_metadata_vio(vio, 0, callback, error_handler,
			    VIO_FLUSH_BEFORE);
}

#endif // VIO_H
