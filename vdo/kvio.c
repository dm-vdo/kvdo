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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvio.c#73 $
 */

#include "kvio.h"


#include "logger.h"
#include "memoryAlloc.h"

#include "numUtils.h"
#include "vdo.h"
#include "vdoInternal.h"
#include "waitQueue.h"

#include "bio.h"
#include "dataKVIO.h"
#include "ioSubmitter.h"
#include "kvdoFlush.h"

/**
 * A function to tell vdo that we have completed the requested async
 * operation for a vio.
 *
 * @param item    The work item of the vio to complete
 **/
static void vdo_handle_vio_callback(struct vdo_work_item *item)
{
	run_callback(container_of(item, struct vdo_completion, work_item));
}

/**********************************************************************/
void enqueue_vio_callback(struct vio *vio)
{
	enqueue_vio(vio,
		    vdo_handle_vio_callback,
		    vio_as_completion(vio)->callback,
		    REQ_Q_ACTION_VIO_CALLBACK);
}

/**********************************************************************/
void continue_vio(struct vio *vio, int error)
{
	if (unlikely(error != VDO_SUCCESS)) {
		set_completion_result(vio_as_completion(vio), error);
	}

	enqueue_vio_callback(vio);
}

/**********************************************************************/
void destroy_vio(struct vio **vio_ptr)
{
	struct vio *vio = *vio_ptr;

	if (vio == NULL) {
		return;
	}

	BUG_ON(is_data_vio(vio));
	free_bio(vio->bio);
	FREE(vio);
	*vio_ptr = NULL;
}


/**********************************************************************/
void write_compressed_block(struct allocating_vio *allocating_vio)
{
	// This method assumes that compressed writes never set the flush or
	// FUA bits.
	struct vio *vio = allocating_vio_as_vio(allocating_vio);
	struct bio *bio = vio->bio;

	// Write the compressed block, using the compressed vio's own bio.
	int result = reset_bio_with_buffer(bio, vio->data, vio,
					   complete_async_bio, REQ_OP_WRITE,
					   vio->physical);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	vdo_submit_bio(bio, BIO_Q_ACTION_COMPRESSED_DATA);
}

/**
 * Get the bio queue action for a metadata vio based on that vio's priority.
 *
 * @param vio  The vio
 *
 * @return The action with which to submit the vio's bio.
 **/
static inline enum bio_q_action get_metadata_action(struct vio *vio)
{
	return ((vio->priority == VIO_PRIORITY_HIGH) ? BIO_Q_ACTION_HIGH :
						       BIO_Q_ACTION_METADATA);
}

/**********************************************************************/
void submit_metadata_vio(struct vio *vio)
{
	int result;
	char *data = vio->data;
	struct bio *bio = vio->bio;
	unsigned int bi_opf;
	struct kernel_layer *layer =
		as_kernel_layer(vio_as_completion(vio)->layer);
	if (is_read_vio(vio)) {
		ASSERT_LOG_ONLY(!vio_requires_flush_before(vio),
				"read vio does not require flush before");
		bi_opf = REQ_OP_READ;
	} else {
		enum kernel_layer_state state = get_kernel_layer_state(layer);
		ASSERT_LOG_ONLY(((state == LAYER_RUNNING)
				 || (state == LAYER_RESUMING)
				 || (state = LAYER_STARTING)),
				"write metadata in allowed state %d", state);
		if (vio_requires_flush_before(vio)) {
			bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;
		} else {
			bi_opf = REQ_OP_WRITE;
		}
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

	result = reset_bio_with_buffer(bio, data, vio,
				       complete_async_bio, bi_opf,
				       vio->physical);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	// Perform the metadata IO, using the metadata vio's own bio.
	vdo_submit_bio(bio, get_metadata_action(vio));
}

/**********************************************************************/
int vdo_create_metadata_vio(PhysicalLayer *layer,
			    enum vio_type vio_type,
			    enum vio_priority priority,
			    void *parent,
			    char *data,
			    struct vio **vio_ptr)
{
	return create_metadata_vio(&(as_kernel_layer(layer)->vdo),
				   vio_type,
				   priority,
				   parent,
				   data,
				   vio_ptr);
}

/**********************************************************************/
int create_compressed_write_vio(struct vdo *vdo,
				void *parent,
				char *data,
				struct allocating_vio **allocating_vio_ptr)
{
	struct kernel_layer *kernel_layer = as_kernel_layer(vdo->layer);
	struct bio *bio;
	struct allocating_vio *allocating_vio;
	struct vio *vio;
	int result = create_bio(&bio);

	if (result != VDO_SUCCESS) {
		return result;
	}

	// Compressed write vios should use direct allocation and not use the
	// buffer pool, which is reserved for submissions from the linux block
	// layer.
	result = ALLOCATE(1, struct allocating_vio, __func__,
			  &allocating_vio);
	if (result != VDO_SUCCESS) {
		uds_log_error("compressed write vio allocation failure %d",
			      result);
		free_bio(bio);
		return result;
	}

	vio = allocating_vio_as_vio(allocating_vio);
	initialize_vio(vio,
		       bio,
		       VIO_TYPE_COMPRESSED_BLOCK,
		       VIO_PRIORITY_COMPRESSED_DATA,
		       parent,
		       &kernel_layer->vdo,
		       data);
	*allocating_vio_ptr = allocating_vio;
	return VDO_SUCCESS;
}
