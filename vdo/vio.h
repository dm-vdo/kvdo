/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VIO_H
#define VIO_H

#include <linux/bio.h>
#include <linux/kernel.h>

#include "bio.h"
#include "completion.h"
#include "constants.h"
#include "kernel-types.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"

enum {
	MAX_BLOCKS_PER_VIO = (BIO_MAX_VECS << PAGE_SHIFT) / VDO_BLOCK_SIZE,
};

/*
 * A representation of a single block which may be passed between the VDO base
 * and the physical layer.
 */
struct vio {
	/* The completion for this vio */
	struct vdo_completion completion;

	/*
	 * The address on the underlying device of the block to be read/written
	 */
	physical_block_number_t physical;

	/* The bio zone in which I/O should be processed */
	zone_count_t bio_zone;

	/* The queueing priority of the vio operation */
	enum vio_priority priority;

	/* The vio type is used for statistics and instrumentation. */
	enum vio_type type;

	/* The size of this vio in blocks */
	unsigned int block_count;

	/* The data being read or written. */
	char *data;

	/* The VDO-owned bio to use for all IO for this vio */
	struct bio *bio;

	/*
	 * A list of enqueued bios with consecutive block numbers, stored by
	 * vdo_submit_bio() under the first-enqueued vio. The other vios are
	 * found via their bio entries in this list, and are not added to
	 * the work queue as separate completions.
	 */
	struct bio_list bios_merged;
};

/**
 * as_vio() - Convert a generic vdo_completion to a vio.
 * @completion: The completion to convert.
 *
 * Return: The completion as a vio.
 */
static inline struct vio *as_vio(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type, VIO_COMPLETION);
	return container_of(completion, struct vio, completion);
}

/**
 * vio_as_completion() - Convert a vio to a generic completion.
 * @vio: The vio to convert.
 *
 * Return: The vio as a completion.
 */
static inline struct vdo_completion *vio_as_completion(struct vio *vio)
{
	return &vio->completion;
}

/**
 * vdo_from_vio() - Get the vdo from a vio.
 * @vio: The vio from which to get the vdo.
 *
 * Return: The vdo to which the vio belongs.
 */
static inline struct vdo *vdo_from_vio(struct vio *vio)
{
	return vio_as_completion(vio)->vdo;
}

/**
 * set_vio_physical() - Set the physical field of a vio.
 * @vio: The vio.
 * @pbn: The pbn to set as the vio's physical address.
 *
 * Also computes the bio zone for doing I/O to that address.
 */
static inline void
set_vio_physical(struct vio *vio, physical_block_number_t pbn)
{
	vio->physical = pbn;
	vio->bio_zone = vdo_get_bio_zone(vdo_from_vio(vio), pbn);
}

/**
 * get_vio_bio_zone_thread_id() - Get the thread id of the bio zone in which a
 *                                vio should submit its I/O.
 * @vio: The vio.
 *
 * Return: The id of the bio zone thread the vio should use.
 */
static inline thread_id_t __must_check
get_vio_bio_zone_thread_id(struct vio *vio)
{
	return vdo_from_vio(vio)->thread_config->bio_threads[vio->bio_zone];
}

/**
 * assert_vio_in_bio_zone() - Check that a vio is running on the correct
 *                            thread for its bio zone.
 * @vio: The vio to check.
 */
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

int __must_check create_multi_block_metadata_vio(struct vdo *vdo,
						 enum vio_type vio_type,
						 enum vio_priority priority,
						 void *parent,
						 unsigned int block_count,
						 char *data,
						 struct vio **vio_ptr);

static inline int __must_check
create_metadata_vio(struct vdo *vdo,
		    enum vio_type vio_type,
		    enum vio_priority priority,
		    void *parent,
		    char *data,
		    struct vio **vio_ptr)
{
	return create_multi_block_metadata_vio(vdo,
					       vio_type,
					       priority,
					       parent,
					       1,
					       data,
					       vio_ptr);
}

void free_vio(struct vio *vio);

/**
 * initialize_vio() - Initialize a vio.
 * @vio: The vio to initialize.
 * @bio: The bio this vio should use for its I/O.
 * @block_count: The size of this vio in vdo blocks.
 * @vio_type: The vio type.
 * @priority: The relative priority of the vio.
 * @vdo: The vdo for this vio.
 */
static inline void initialize_vio(struct vio *vio,
		    struct bio *bio,
		    unsigned int block_count,
		    enum vio_type vio_type,
		    enum vio_priority priority,
		    struct vdo *vdo)
{
	vio->bio = bio;
	vio->block_count = block_count;
	vio->type = vio_type;
	vio->priority = priority;
	vdo_initialize_completion(vio_as_completion(vio), vdo, VIO_COMPLETION);
}

void update_vio_error_stats(struct vio *vio, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/**
 * is_data_vio() - Check whether a vio is servicing an external data request.
 * @vio: The vio to check.
 */
static inline bool is_data_vio(struct vio *vio)
{
	return vdo_is_data_vio_type(vio->type);
}

/**
 * is_metadata_vio() - Check whether a vio is for metadata
 * @vio: The vio to check.
 */
static inline bool is_metadata_vio(struct vio *vio)
{
	return vdo_is_metadata_vio_type(vio->type);
}

/**
 * get_metadata_priority() - Convert a vio's priority to a work item priority.
 * @vio: The vio.
 *
 * Return: The priority with which to submit the vio's bio.
 */
static inline enum vdo_completion_priority
get_metadata_priority(struct vio *vio)
{
	return ((vio->priority == VIO_PRIORITY_HIGH)
		? BIO_Q_HIGH_PRIORITY : BIO_Q_METADATA_PRIORITY);
}

/**
 * prepare_vio_for_io() - Reset a vio's bio to prepare for issuing I/O.
 * @vio: The vio preparing to issue I/O.
 * @data: The buffer the bio should wrap.
 * @callback: The callback the bio should call when IO finishes.
 * @bi_opf: The operation and flags for the bio.
 *
 * The pbn to which the I/O will be directed is taken from the 'physical'
 * field of the vio.
 *
 * Return: VDO_SUCCESS or an error.
 */
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
 * continue_vio() - Enqueue a vio to run its next callback.
 * @vio: The vio to continue.
 *
 * Return: The result of the current operation.
 */
static inline void continue_vio(struct vio *vio, int result)
{
	struct vdo_completion *completion = vio_as_completion(vio);

	if (unlikely(result != VDO_SUCCESS)) {
		vdo_set_completion_result(vio_as_completion(vio), result);
	}

	vdo_enqueue_completion(completion);
}

/**
 * continue_vio_after_io() - Continue a vio now that its I/O has returned.
 */
static inline void continue_vio_after_io(struct vio *vio,
					 vdo_action *callback,
					 thread_id_t thread)
{
	vdo_count_completed_bios(vio->bio);
	vdo_set_completion_callback(vio_as_completion(vio), callback, thread);
	continue_vio(vio, vdo_get_bio_result(vio->bio));
}

void record_metadata_io_error(struct vio *vio);

#endif /* VIO_H */
