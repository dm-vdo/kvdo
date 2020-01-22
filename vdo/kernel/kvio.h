/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvio.h#18 $
 */

#ifndef KVIO_H
#define KVIO_H

#include "allocatingVIO.h"
#include "vio.h"

#include "kernelLayer.h"

/**
 * A specific (semi-opaque) encapsulation of a single block
 **/
struct kvio {
	struct kvdo_enqueueable enqueueable;
	struct vio *vio;
	struct kernel_layer *layer;
	struct bio *bio;

	/**
	 * A bio pointer used in enqueueBioMap (used via vdo_submit_bio etc),
	 * to pass information -- which bio to submit to the storage device --
	 * across a thread switch. This may match another bio pointer in
	 * this structure, or could point somewhere else.
	 **/
	struct bio *bio_to_submit;
	/**
	 * A list of enqueued bios with consecutive block numbers, stored by
	 * enqueueBioMap under the first-enqueued kvio. The other KVIOs are
	 * found via their bio entries in this list, and are not added to
	 * the work queue as separate work items.
	 **/
	struct bio_list bios_merged;
	/** A slot for an arbitrary bit of data, for use by systemtap. */
	long debug_slot;
};

struct metadata_kvio {
	struct kvio kvio;
	struct vio vio;
};

struct compressed_write_kvio {
	struct kvio           kvio;
	struct allocating_vio allocating_vio;
};

/**
 * Determine whether a kvio is a data vio or not
 *
 * @param kvio  The kvio to check
 *
 * @return <code>true</code> if a data kvio
 */
static inline bool is_data(struct kvio *kvio)
{
	return isDataVIO(kvio->vio);
}

/**
 * Determine whether a kvio is a compressed block write vio or not
 *
 * @param kvio  The kvio to check
 *
 * @return <code>true</code> if a compressed block writer
 */
static inline bool is_compressed_writer(struct kvio *kvio)
{
	return isCompressedWriteVIO(kvio->vio);
}

/**
 * Determine whether a kvio is a metadata vio or not
 *
 * @param kvio  The kvio to check
 *
 * @return <code>true</code> if a metadata kvio
 */
static inline bool is_metadata(struct kvio *kvio)
{
	return isMetadataVIO(kvio->vio);
}

/**
 * Convert a vio to a struct metadata_kvio.
 *
 * @param vio  The vio to convert
 *
 * @return the vio as a metadata_kvio
 **/
static inline struct metadata_kvio *vio_as_metadata_kvio(struct vio *vio)
{
	ASSERT_LOG_ONLY(isMetadataVIO(vio), "vio is a metadata vio");
	return container_of(vio, struct metadata_kvio, vio);
}

/**
 * Convert a metadata_kvio to a kvio.
 *
 * @param metadata_kvio  The metadata_kvio to convert
 *
 * @return The metadata_kvio as a kvio
 **/
static inline struct kvio *
metadata_kvio_as_kvio(struct metadata_kvio *metadata_kvio)
{
	return &metadata_kvio->kvio;
}

/**
 * Returns a pointer to the struct compressed_write_kvio wrapping an
 * allocating_vio.
 *
 * @param allocating_vio  The allocating_vio to convert
 *
 * @return the struct compressed_write_kvio
 **/
static inline struct compressed_write_kvio *
allocating_vio_as_compressed_write_kvio(struct allocating_vio *allocating_vio)
{
	ASSERT_LOG_ONLY(is_compressed_write_allocating_vio(allocating_vio),
			"struct allocating_vio is a compressed write");
	return container_of(allocating_vio, struct compressed_write_kvio,
			    allocating_vio);
}

/**
 * Convert a compressed_write_kvio to a kvio.
 *
 * @param compressed_write_kvio  The compressed_write_kvio to convert
 *
 * @return The compressed_write_kvio as a kvio
 **/
static inline struct kvio *
compressed_write_kvio_as_kvio(struct compressed_write_kvio *compressed_write_kvio)
{
	return &compressed_write_kvio->kvio;
}

/**
 * Returns a pointer to the kvio wrapping a work item
 *
 * @param item  the work item
 *
 * @return the kvio
 **/
static inline struct kvio *work_item_as_kvio(struct kvdo_work_item *item)
{
	return container_of(item, struct kvio, enqueueable.work_item);
}

/**
 * Enqueue a kvio on a work queue.
 *
 * @param queue  The queue
 * @param kvio   The kvio
 **/
static inline void enqueue_kvio_work(struct kvdo_work_queue *queue,
				     struct kvio *kvio)
{
	enqueue_work_queue(queue, &kvio->enqueueable.work_item);
}

/**
 * Add a trace record for the current source location.
 *
 * @param kvio      The kvio structure to be updated
 * @param location  The source-location descriptor to be recorded
 **/
static inline void kvio_add_trace_record(struct kvio *kvio,
					 TraceLocation location)
{
	vioAddTraceRecord(kvio->vio, location);
}

/**
 * Set up the work item for a kvio.
 *
 * @param kvio            The kvio to set up
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 **/
static inline void setup_kvio_work(struct kvio *kvio,
				   KvdoWorkFunction work,
				   void *stats_function,
				   unsigned int action)
{
	setup_work_item(&kvio->enqueueable.work_item,
			work,
			stats_function,
			action);
}

/**
 * Set up and enqueue a kvio.
 *
 * @param kvio            The kvio to set up
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 * @param queue           The queue on which to enqueue the kvio
 **/
static inline void launch_kvio(struct kvio *kvio,
			       KvdoWorkFunction work,
			       void *stats_function,
			       unsigned int action,
			       struct kvdo_work_queue *queue)
{
	setup_kvio_work(kvio, work, stats_function, action);
	enqueue_kvio_work(queue, kvio);
}

/**
 * Move a kvio back to the base threads.
 *
 * @param kvio The kvio to enqueue
 **/
void kvdo_enqueue_vio_callback(struct kvio *kvio);

/**
 * Handles kvio-related I/O post-processing.
 *
 * @param kvio  The kvio to finalize
 * @param error Possible error
 **/
void kvdo_continue_kvio(struct kvio *kvio, int error);

/**
 * Initialize a kvio structure.
 *
 * @param kvio       The kvio to initialize
 * @param layer      The physical layer
 * @param vio_type   The type of vio to create
 * @param priority   The relative priority to assign to the kvio
 * @param parent     The parent of the kvio completion
 * @param bio        The bio to associate with this kvio
 **/
void initialize_kvio(struct kvio *kvio,
		     struct kernel_layer *layer,
		     VIOType vio_type,
		     VIOPriority priority,
		     void *parent,
		     struct bio *bio);

/**
 * Destroy a struct metadata_kvio and NULL out the pointer to it.
 *
 * @param metadata_kvio_ptr  A pointer to the struct metadata_kvio to destroy
 **/
void free_metadata_kvio(struct metadata_kvio **metadata_kvio_ptr);

/**
 * Destroy a struct compressed_write_kvio and NULL out the pointer to it.
 *
 * @param compressed_write_kvio_ptr  A pointer to the compressed_write_kvio to
 *                                   destroy
 **/
void
free_compressed_write_kvio(struct compressed_write_kvio **compressed_write_kvio_ptr);

/**
 * Create a new vio (and its enclosing kvio) for metadata operations.
 *
 * <p>Implements MetadataVIOCreator.
 *
 * @param [in]  layer      The physical layer
 * @param [in]  vio_type   The type of vio to create
 * @param [in]  priority   The relative priority to assign to the vio
 * @param [in]  parent     The parent to assign to the vio's completion
 * @param [in]  data       The buffer
 * @param [out] vio_ptr    A pointer to hold new vio
 *
 * @return VDO_SUCCESS or an error
 **/
int kvdo_create_metadata_vio(PhysicalLayer *layer,
			     VIOType vio_type,
			     VIOPriority priority,
			     void *parent,
			     char *data,
			     struct vio **vio_ptr)
	__attribute__((warn_unused_result));

/**
 * Create a new allocating_vio (and its enclosing kvio) for compressed writes.
 *
 * <p>Implements CompressedWriteVIOCreator.
 *
 * @param [in]  layer              The physical layer
 * @param [in]  parent             The parent to assign to the allocating_vio's
 *                                 completion
 * @param [in]  data               The buffer
 * @param [out] allocating_vio_ptr  A pointer to hold new allocating_vio
 *
 * @return VDO_SUCCESS or an error
 **/
int kvdo_create_compressed_write_vio(PhysicalLayer *layer,
				     void *parent,
				     char *data,
				     struct allocating_vio **allocating_vio_ptr)
	__attribute__((warn_unused_result));

/**
 * Issue an empty flush to the lower layer using the bio in a metadata vio.
 *
 * <p>Implements MetadataWriter.
 *
 * @param vio  The vio to flush
 **/
void kvdo_flush_vio(struct vio *vio);

#endif /* KVIO_H */
