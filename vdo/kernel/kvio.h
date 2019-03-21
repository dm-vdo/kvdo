/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvio.h#11 $
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
  KvdoEnqueueable    enqueueable;
  VIO               *vio;
  KernelLayer       *layer;
  struct bio        *bio;

  /**
   * A bio pointer used in enqueueBioMap (used via submitBio etc), to
   * pass information -- which bio to submit to the storage device --
   * across a thread switch. This may match another bio pointer in
   * this structure, or could point somewhere else.
   **/
  struct bio        *bioToSubmit;
  /**
   * A list of enqueued bios with consecutive block numbers, stored by
   * enqueueBioMap under the first-enqueued kvio. The other KVIOs are
   * found via their bio entries in this list, and are not added to
   * the work queue as separate work items.
   **/
  struct bio_list    biosMerged;
  /** A slot for an arbitrary bit of data, for use by systemtap. */
  long               debugSlot;
};

struct metadata_kvio {
  struct kvio kvio;
  VIO         vio;
};

struct compressed_write_kvio {
  struct kvio   kvio;
  AllocatingVIO allocatingVIO;
};

/**
 * Determine whether a kvio is a data VIO or not
 *
 * @param kvio  The kvio to check
 *
 * @return <code>true</code> if a data kvio
 */
static inline bool isData(struct kvio *kvio)
{
  return isDataVIO(kvio->vio);
}

/**
 * Determine whether a kvio is a compressed block write VIO or not
 *
 * @param kvio  The kvio to check
 *
 * @return <code>true</code> if a compressed block writer
 */
static inline bool isCompressedWriter(struct kvio *kvio)
{
  return isCompressedWriteVIO(kvio->vio);
}

/**
 * Determine whether a kvio is a metadata VIO or not
 *
 * @param kvio  The kvio to check
 *
 * @return <code>true</code> if a metadata kvio
 */
static inline bool isMetadata(struct kvio *kvio)
{
  return isMetadataVIO(kvio->vio);
}

/**
 * Convert a VIO to a struct metadata_kvio.
 *
 * @param vio  The VIO to convert
 *
 * @return the VIO as a metadata_kvio
 **/
static inline struct metadata_kvio *vioAsMetadataKVIO(VIO *vio)
{
  ASSERT_LOG_ONLY(isMetadataVIO(vio), "VIO is a metadata VIO");
  return container_of(vio, struct metadata_kvio, vio);
}

/**
 * Convert a metadata_kvio to a kvio.
 *
 * @param metadataKVIO  The metadata_kvio to convert
 *
 * @return The metadata_kvio as a kvio
 **/
static inline struct kvio *
metadataKVIOAsKVIO(struct metadata_kvio *metadataKVIO)
{
  return &metadataKVIO->kvio;
}

/**
 * Returns a pointer to the struct compressed_write_kvio wrapping an AllocatingVIO.
 *
 * @param allocatingVIO  The AllocatingVIO to convert
 *
 * @return the struct compressed_write_kvio
 **/
static inline struct compressed_write_kvio *
allocatingVIOAsCompressedWriteKVIO(AllocatingVIO *allocatingVIO)
{
  ASSERT_LOG_ONLY(isCompressedWriteAllocatingVIO(allocatingVIO),
                  "AllocatingVIO is a compressed write");
  return container_of(allocatingVIO, struct compressed_write_kvio,
                      allocatingVIO);
}

/**
 * Convert a compressed_write_kvio to a kvio.
 *
 * @param compressedWriteKVIO  The compressed_write_kvio to convert
 *
 * @return The compressed_write_kvio as a kvio
 **/
static inline struct kvio *
compressedWriteKVIOAsKVIO(struct compressed_write_kvio *compressedWriteKVIO)
{
  return &compressedWriteKVIO->kvio;
}

/**
 * Returns a pointer to the kvio wrapping a work item
 *
 * @param item  the work item
 *
 * @return the kvio
 **/
static inline struct kvio *workItemAsKVIO(struct kvdo_work_item *item)
{
  return container_of(item, struct kvio, enqueueable.workItem);
}

/**
 * Enqueue a kvio on a work queue.
 *
 * @param queue  The queue
 * @param kvio   The kvio
 **/
static inline void enqueueKVIOWork(struct kvdo_work_queue *queue,
                                   struct kvio *kvio)
{
  enqueue_work_queue(queue, &kvio->enqueueable.workItem);
}

/**
 * Add a trace record for the current source location.
 *
 * @param kvio      The kvio structure to be updated
 * @param location  The source-location descriptor to be recorded
 **/
static inline void kvioAddTraceRecord(struct kvio *kvio,
                                      TraceLocation location)
{
  vioAddTraceRecord(kvio->vio, location);
}

/**
 * Set up the work item for a kvio.
 *
 * @param kvio           The kvio to set up
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
static inline void setupKVIOWork(struct kvio      *kvio,
                                 KvdoWorkFunction  work,
                                 void             *statsFunction,
                                 unsigned int      action)
{
  setup_work_item(&kvio->enqueueable.workItem, work, statsFunction, action);
}

/**
 * Set up and enqueue a kvio.
 *
 * @param kvio           The kvio to set up
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 * @param queue          The queue on which to enqueue the kvio
 **/
static inline void launchKVIO(struct kvio	        *kvio,
                              KvdoWorkFunction		 work,
                              void			*statsFunction,
                              unsigned int		 action,
                              struct kvdo_work_queue    *queue)
{
  setupKVIOWork(kvio, work, statsFunction, action);
  enqueueKVIOWork(queue, kvio);
}

/**
 * Move a kvio back to the base threads.
 *
 * @param kvio The kvio to enqueue
 **/
void kvdoEnqueueVIOCallback(struct kvio *kvio);

/**
 * Handles kvio-related I/O post-processing.
 *
 * @param kvio  The kvio to finalize
 * @param error Possible error
 **/
void kvdoContinueKvio(struct kvio *kvio, int error);

/**
 * Initialize a kvio structure.
 *
 * @param kvio      The kvio to initialize
 * @param layer     The physical layer
 * @param vioType   The type of VIO to create
 * @param priority  The relative priority to assign to the kvio
 * @param parent    The parent of the kvio completion
 * @param bio       The bio to associate with this kvio
 **/
void initializeKVIO(struct kvio *kvio,
                    KernelLayer *layer,
                    VIOType      vioType,
                    VIOPriority  priority,
                    void        *parent,
                    struct bio  *bio);

/**
 * Destroy a struct metadata_kvio and NULL out the pointer to it.
 *
 * @param metadataKVIOPtr  A pointer to the struct metadata_kvio to destroy
 **/
void freeMetadataKVIO(struct metadata_kvio **metadataKVIOPtr);

/**
 * Destroy a struct compressed_write_kvio and NULL out the pointer to it.
 *
 * @param compressedWriteKVIOPtr  A pointer to the compressed_write_kvio to
 *                                destroy
 **/
void freeCompressedWriteKVIO(struct compressed_write_kvio **compressedWriteKVIOPtr);

/**
 * Create a new VIO (and its enclosing kvio) for metadata operations.
 *
 * <p>Implements MetadataVIOCreator.
 *
 * @param [in]  layer     The physical layer
 * @param [in]  vioType   The type of VIO to create
 * @param [in]  priority  The relative priority to assign to the VIO
 * @param [in]  parent    The parent to assign to the VIO's completion
 * @param [in]  data      The buffer
 * @param [out] vioPtr    A pointer to hold new VIO
 *
 * @return VDO_SUCCESS or an error
 **/
int kvdoCreateMetadataVIO(PhysicalLayer  *layer,
                          VIOType         vioType,
                          VIOPriority     priority,
                          void           *parent,
                          char           *data,
                          VIO           **vioPtr)
  __attribute__((warn_unused_result));

/**
 * Create a new AllocatingVIO (and its enclosing kvio) for compressed writes.
 *
 * <p>Implements CompressedWriteVIOCreator.
 *
 * @param [in]  layer             The physical layer
 * @param [in]  parent            The parent to assign to the AllocatingVIO's
 *                                completion
 * @param [in]  data              The buffer
 * @param [out] allocatingVIOPtr  A pointer to hold new AllocatingVIO
 *
 * @return VDO_SUCCESS or an error
 **/
int kvdoCreateCompressedWriteVIO(PhysicalLayer  *layer,
                                 void           *parent,
                                 char           *data,
                                 AllocatingVIO **allocatingVIOPtr)
  __attribute__((warn_unused_result));

/**
 * Issue an empty flush to the lower layer using the bio in a metadata VIO.
 *
 * <p>Implements MetadataWriter.
 *
 * @param vio  The VIO to flush
 **/
void kvdoFlushVIO(VIO *vio);

#endif /* KVIO_H */
