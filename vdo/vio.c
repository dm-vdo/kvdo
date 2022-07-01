// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vio.h"

#include <linux/kernel.h>
#include <linux/ratelimit.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "bio.h"
#include "io-submitter.h"
#include "vdo.h"

/**
 * create_multi_block_metadata_vio() - Create a vio.
 * @vdo: The vdo on which the vio will operate.
 * @vio_type: The type of vio to create.
 * @priority: The relative priority to assign to the vio.
 * @parent: The parent of the vio.
 * @block_count: The size of the vio in blocks.
 * @data: The buffer.
 * @vio_ptr: A pointer to hold the new vio.
 *
 * Return: VDO_SUCCESS or an error.
 */
int create_multi_block_metadata_vio(struct vdo *vdo,
				    enum vio_type vio_type,
				    enum vio_priority priority,
				    void *parent,
				    unsigned int block_count,
				    char *data,
				    struct vio **vio_ptr)
{
	struct vio *vio;
	struct bio *bio;
	int result;

	/*
	 * If struct vio grows past 256 bytes, we'll lose benefits of
	 * VDOSTORY-176.
	 */
	STATIC_ASSERT(sizeof(struct vio) <= 256);

	result = ASSERT(block_count <= MAX_BLOCKS_PER_VIO,
			"block count %u does not exceed maximum %u",
			block_count,
			MAX_BLOCKS_PER_VIO);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ASSERT(vdo_is_metadata_vio_type(vio_type),
			"%d is a metadata type",
			vio_type);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/*
	 * Metadata vios should use direct allocation and not use the buffer
	 * pool, which is reserved for submissions from the linux block layer.
	 */
	result = UDS_ALLOCATE(1, struct vio, __func__, &vio);
	if (result != VDO_SUCCESS) {
		uds_log_error("metadata vio allocation failure %d", result);
		return result;
	}

	result = vdo_create_multi_block_bio(block_count, &bio);
	if (result != VDO_SUCCESS) {
		UDS_FREE(vio);
		return result;
	}

	initialize_vio(vio,
		       bio,
		       block_count,
		       vio_type,
		       priority,
		       vdo);
	vio->completion.parent = parent;
	vio->data = data;
	*vio_ptr  = vio;
	return VDO_SUCCESS;
}

/**
 * free_vio() - Destroy a vio.
 * @vio: The vio to destroy.
 */
void free_vio(struct vio *vio)
{
	if (vio == NULL) {
		return;
	}

	BUG_ON(is_data_vio(vio));
	vdo_free_bio(UDS_FORGET(vio->bio));
	UDS_FREE(vio);
}

/**
 * update_vio_error_stats() - Update per-vio error stats and log the
 *                            error.
 * @vio: The vio which got an error.
 * @format: The format of the message to log (a printf style format).
 */
void update_vio_error_stats(struct vio *vio, const char *format, ...)
{
	static DEFINE_RATELIMIT_STATE(error_limiter,
				      DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	va_list args;
	int priority;
	struct vdo_completion *completion = vio_as_completion(vio);

	switch (completion->result) {
	case VDO_READ_ONLY:
		atomic64_inc(&completion->vdo->stats.read_only_error_count);
		return;

	case VDO_NO_SPACE:
		atomic64_inc(&completion->vdo->stats.no_space_error_count);
		priority = UDS_LOG_DEBUG;
		break;

	default:
		priority = UDS_LOG_ERR;
	}

	if (!__ratelimit(&error_limiter)) {
		return;
	}

	va_start(args, format);
	uds_vlog_strerror(priority,
			  completion->result,
			  UDS_LOGGING_MODULE_NAME,
			  format,
			  args);
	va_end(args);
}

void record_metadata_io_error(struct vio *vio)
{
	const char *description;

	if (bio_op(vio->bio) == REQ_OP_READ) {
		description = "read";
	} else if ((vio->bio->bi_opf & REQ_PREFLUSH) == REQ_PREFLUSH) {
		description = (((vio->bio->bi_opf & REQ_FUA) == REQ_FUA) ?
			       "write+preflush+fua" :
			       "write+preflush");
	} else if ((vio->bio->bi_opf & REQ_FUA) == REQ_FUA) {
		description = "write+fua";
	} else {
		description = "write";
	}

	update_vio_error_stats(vio,
			       "Completing %s vio of type %u for physical block %llu with error",
			       description,
			       vio->type,
			       (unsigned long long) vio->physical);
}
