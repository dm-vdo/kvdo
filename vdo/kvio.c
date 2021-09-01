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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvio.c#92 $
 */

#include "kvio.h"


#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "numUtils.h"
#include "vdo.h"
#include "waitQueue.h"

#include "bio.h"
#include "dataKVIO.h"
#include "ioSubmitter.h"

/**
 * A function to tell vdo that we have completed the requested async
 * operation for a vio.
 *
 * @param item  The work item of the vio to complete
 **/
static void vdo_handle_vio_callback(struct vdo_work_item *item)
{
	run_vdo_completion_callback(container_of(item, struct vdo_completion,
				    work_item));
}

/**********************************************************************/
void enqueue_vio_callback(struct vio *vio)
{
	enqueue_vio(vio,
		    vdo_handle_vio_callback,
		    vio_as_completion(vio)->callback,
		    VDO_REQ_Q_VIO_CALLBACK_PRIORITY);
}

/**********************************************************************/
void continue_vio(struct vio *vio, int error)
{
	if (unlikely(error != VDO_SUCCESS)) {
		set_vdo_completion_result(vio_as_completion(vio), error);
	}

	enqueue_vio_callback(vio);
}

/**********************************************************************/
void write_compressed_block_vio(struct vio *vio)
{
	// This method assumes that compressed writes never set the flush or
	// FUA bits.
	struct bio *bio = vio->bio;
	int result = ASSERT(is_compressed_write_vio(vio),
			    "Compressed write vio has correct type");
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	// Write the compressed block, using the compressed vio's own bio.
	result = vdo_reset_bio_with_buffer(bio,
					   vio->data,
					   vio,
					   vdo_complete_async_bio,
					   REQ_OP_WRITE,
					   vio->physical);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	vdo_submit_bio(bio, BIO_Q_COMPRESSED_DATA_PRIORITY);
}

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

/**********************************************************************/
void submit_metadata_vio(struct vio *vio)
{
	int result;
	char *data = vio->data;
	struct bio *bio = vio->bio;
	unsigned int bi_opf;

	if (is_read_vio(vio)) {
		ASSERT_LOG_ONLY(!vio_requires_flush_before(vio),
				"read vio does not require flush before");
		bi_opf = REQ_OP_READ;
	} else if (vio_requires_flush_before(vio)) {
		bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;
	} else {
		bi_opf = REQ_OP_WRITE;
	}

	if (vio_requires_flush_after(vio)) {
		bi_opf |= REQ_FUA;
	}

	/*
	 * Everything coming through this function is metadata, so flag it as
	 * REQ_META in case the lower layers benefit from that information.
	 *
	 * We believe all recovery journal and block map IO is important for
	 * throughput relative to other IO, so we tag them with REQ_PRIO to
	 * convey this to lower layers, if they care.
	 *
	 * Additionally, recovery journal IO is directly critical to user
	 * bio latency, so we tag them with REQ_SYNC.
	 **/
	bi_opf |= REQ_META;
	if ((vio->type == VIO_TYPE_BLOCK_MAP_INTERIOR) ||
	    (vio->type == VIO_TYPE_BLOCK_MAP) ||
	    (vio->type == VIO_TYPE_RECOVERY_JOURNAL)) {
		bi_opf |= REQ_PRIO;
	}

	if (vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
		bi_opf |= REQ_SYNC;
	}

	if (is_empty_flush_vio(vio)) {
		data = NULL;
	}

	result = vdo_reset_bio_with_buffer(bio, data, vio,
					   vdo_complete_async_bio, bi_opf,
					   vio->physical);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	// Perform the metadata IO, using the metadata vio's own bio.
	vdo_submit_bio(bio, get_metadata_priority(vio));
}
