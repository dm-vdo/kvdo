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
 */

#ifndef VIO_H
#define VIO_H

#include <linux/kernel.h>

#include "bio.h"
#include "completion.h"
#include "kernel-types.h"
#include "thread-config.h"
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

	/**
	 * The address on the underlying device of the block to be read/written
	 **/
	physical_block_number_t physical;

	/** The bio zone in which I/O should be processed */
	zone_count_t bio_zone;

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
	 * vdo_submit_bio() under the first-enqueued vio. The other vios are
	 * found via their bio entries in this list, and are not added to
	 * the work queue as separate work items.
	 **/
	struct bio_list bios_merged;
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
	assert_vdo_completion_type(completion->type, VIO_COMPLETION);
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
 * Get the vdo from a vio.
 *
 * @param vio  The vio from which to get the vdo
 *
 * @return The vdo to which the vio belongs
 **/
static inline struct vdo *get_vdo_from_vio(struct vio *vio)
{
	return vio_as_completion(vio)->vdo;
}

/**
 * Set the physical field of a vio. Also computes the bio zone for doing I/O
 * to that address.
 *
 * @param vio  The vio
 * @param pbn  The pbn to set as the vio's physical address
 **/
static inline void
set_vio_physical(struct vio *vio, physical_block_number_t pbn)
{
	vio->physical = pbn;
	vio->bio_zone = get_vdo_bio_zone(get_vdo_from_vio(vio), pbn);
}

/**
 * Get the thread id of the bio zone in which a vio should submit its I/O.
 *
 * @param vio  The vio
 *
 * @return The id of the bio zone thread the vio should use
 **/
static inline thread_id_t __must_check
get_vio_bio_zone_thread_id(struct vio *vio)
{
	return get_vdo_from_vio(vio)->thread_config->bio_threads[vio->bio_zone];
}

/**
 * Check that a vio is running on the correct thread for its bio zone.
 *
 * @param vio  The vio to check
 **/
static inline void
assert_vio_in_bio_zone(struct vio *vio)
{
	thread_id_t expected = get_vio_bio_zone_thread_id(vio);
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((expected == thread_id),
			"vio I/O for physical block %llu on thread %u, should be on bio zone thread %u",
			(unsigned long long) vio->physical,
			thread_id,
			expected);
}

int __must_check create_metadata_vio(struct vdo *vdo,
				     enum vio_type vio_type,
				     enum vio_priority priority,
				     void *parent,
				     char *data,
				     struct vio **vio_ptr);

void free_vio(struct vio *vio);

void initialize_vio(struct vio *vio,
		    struct bio *bio,
		    enum vio_type vio_type,
		    enum vio_priority priority,
		    struct vdo_completion *parent,
		    struct vdo *vdo,
		    char *data);

void vio_done_callback(struct vdo_completion *completion);

void get_vio_operation_description(const struct vio *vio, char *buffer);

void update_vio_error_stats(struct vio *vio, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/**
 * Check whether a vio is servicing an external data request.
 *
 * @param vio  The vio to check
 **/
static inline bool is_data_vio(struct vio *vio)
{
	return is_vdo_data_vio_type(vio->type);
}

/**
 * Check whether a vio is for compressed block writes
 *
 * @param vio  The vio to check
 **/
static inline bool is_compressed_write_vio(struct vio *vio)
{
	return is_vdo_compressed_write_vio_type(vio->type);
}

/**
 * Check whether a vio is for metadata
 *
 * @param vio  The vio to check
 **/
static inline bool is_metadata_vio(struct vio *vio)
{
	return is_vdo_metadata_vio_type(vio->type);
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
 * Issue a flush to the layer.
 *
 * @param vio            The vio to notify when the flush is complete
 * @param callback       The function to call when the flush is complete
 * @param error_handler  The handler for flush errors
 **/
static inline void launch_flush_vio(struct vio *vio,
				    vdo_action *callback,
				    vdo_action *error_handler)
{
	launch_metadata_vio(vio, 0, callback, error_handler,
			    VIO_FLUSH_BEFORE);
}

/**
 * Read or write a single metadata vio.
 *
 * @param vio  The vio to read or write
 **/
void submit_metadata_vio(struct vio *vio);

/**
 * A function to write a single compressed block to the layer
 *
 * @param vio  The compressed write vio to write
 **/
void write_compressed_block_vio(struct vio *vio);

/**
 * Convert a vio's priority to a work item priority.
 *
 * @param vio  The vio
 *
 * @return The priority with which to submit the vio's bio.
 **/
static inline enum vdo_work_item_priority
get_metadata_priority(struct vio *vio)
{
	return ((vio->priority == VIO_PRIORITY_HIGH)
		? BIO_Q_HIGH_PRIORITY : BIO_Q_METADATA_PRIORITY);
}

/**
 * Reset a vio's bio to prepare for issuing I/O. The pbn to which the I/O will
 * be directed is taken from the 'physical' field of the vio.
 *
 * @param vio       The vio preparing to issue I/O
 * @param data      The buffer the bio should wrap
 * @param callback  The callback the bio should call when IO finishes
 * @param bi_opf    The operation and flags for the bio
 *
 * @return VDO_SUCCESS or an error
 **/
static inline int __must_check
prepare_vio_for_io(struct vio *vio,
		   char *data,
		   bio_end_io_t callback,
		   unsigned int bi_opf)
{
	return vdo_reset_bio_with_buffer(vio->bio,
					 data,
					 vio,
					 callback,
					 bi_opf,
					 vio->physical);
}


/**
 * Enqueue a vio to run its next callback.
 *
 * @param vio     The vio to continue
 * @param result  The result of the current operation
 **/
static inline void continue_vio(struct vio *vio, int result)
{
	struct vdo_completion *completion = vio_as_completion(vio);

	if (unlikely(result != VDO_SUCCESS)) {
		set_vdo_completion_result(vio_as_completion(vio), result);
	}

	enqueue_vdo_completion(completion);
}

#endif /* VIO_H */
