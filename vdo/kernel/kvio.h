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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/kvio.h#1 $
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
  BIO               *bio;

  /**
   * A bio pointer used in enqueueBioMap (used via submitBio etc), to
   * pass information -- which bio to submit to the storage device --
   * across a thread switch. This may match another bio pointer in
   * this structure, or could point somewhere else (e.g., a read cache
   * entry's buffer).
   **/
  BIO               *bioToSubmit;
  /**
   * A list of enqueued bios with consecutive block numbers, stored by
   * enqueueBioMap under the first-enqueued KVIO. The other KVIOs are
   * found via their bio entries in this list, and are not added to
   * the work queue as separate work items.
   **/
  struct bio_list    biosMerged;
  /**
   * An operation to be invoked via enqueueBioMap in the appropriate
   * worker thread, after suitable reordering.
   *
   * The callback is applied to field enqueueable.workItem, so it can
   * locate the containing KVIO like any other work function.
   **/
  KvdoWorkFunction   bioSubmissionCallback;
  /** A slot for an arbitrary bit of data, for use by systemtap. */
  long               debugSlot;
};

typedef struct {
  KVIO kvio;
  VIO  vio;
} MetadataKVIO;

typedef struct {
  KVIO          kvio;
  AllocatingVIO allocatingVIO;
} CompressedWriteKVIO;

/**
 * Determine whether a KVIO is a data VIO or not
 *
 * @param kvio  The KVIO to check
 *
 * @return <code>true</code> if a data KVIO
 */
static inline bool isData(KVIO *kvio)
{
  return isDataVIO(kvio->vio);
}

/**
 * Determine whether a KVIO is a compressed block write VIO or not
 *
 * @param kvio  The KVIO to check
 *
 * @return <code>true</code> if a compressed block writer
 */
static inline bool isCompressedWriter(KVIO *kvio)
{
  return isCompressedWriteVIO(kvio->vio);
}

/**
 * Determine whether a KVIO is a metadata VIO or not
 *
 * @param kvio  The KVIO to check
 *
 * @return <code>true</code> if a metadata KVIO
 */
static inline bool isMetadata(KVIO *kvio)
{
  return isMetadataVIO(kvio->vio);
}

/**
 * Convert a VIO to a MetadataKVIO.
 *
 * @param vio  The VIO to convert
 *
 * @return the VIO as a KVIO
 **/
static inline MetadataKVIO *vioAsMetadataKVIO(VIO *vio)
{
  ASSERT_LOG_ONLY(isMetadataVIO(vio), "VIO is a metadata VIO");
  return container_of(vio, MetadataKVIO, vio);
}

/**
 * Convert a MetadataKVIO to a KVIO.
 *
 * @param metadataKVIO  The MetadataKVIO to convert
 *
 * @return The MetadataKVIO as a KVIO
 **/
static inline KVIO *metadataKVIOAsKVIO(MetadataKVIO *metadataKVIO)
{
  return &metadataKVIO->kvio;
}

/**
 * Returns a pointer to the CompressedWriteKVIO wrapping an AllocatingVIO.
 *
 * @param allocatingVIO  The AllocatingVIO to convert
 *
 * @return the CompressedWriteKVIO
 **/
static inline CompressedWriteKVIO *
allocatingVIOAsCompressedWriteKVIO(AllocatingVIO *allocatingVIO)
{
  ASSERT_LOG_ONLY(isCompressedWriteAllocatingVIO(allocatingVIO),
                  "AllocatingVIO is a compressed write");
  return container_of(allocatingVIO, CompressedWriteKVIO, allocatingVIO);
}

/**
 * Convert a CompressedWriteKVIO to a KVIO.
 *
 * @param compressedWriteKVIO  The CompressedWriteKVIO to convert
 *
 * @return The CompressedWriteKVIO as a KVIO
 **/
static inline
KVIO *compressedWriteKVIOAsKVIO(CompressedWriteKVIO *compressedWriteKVIO)
{
  return &compressedWriteKVIO->kvio;
}

/**
 * Returns a pointer to the KVIO wrapping a work item
 *
 * @param item  the work item
 *
 * @return the KVIO
 **/
static inline KVIO *workItemAsKVIO(KvdoWorkItem *item)
{
  return container_of(item, KVIO, enqueueable.workItem);
}

/**
 * Enqueue a KVIO on a work queue.
 *
 * @param queue  The queue
 * @param kvio   The KVIO
 **/
static inline void enqueueKVIOWork(KvdoWorkQueue *queue, KVIO *kvio)
{
  enqueueWorkQueue(queue, &kvio->enqueueable.workItem);
}

/**
 * Add a trace record for the current source location.
 *
 * @param kvio      The KVIO structure to be updated
 * @param location  The source-location descriptor to be recorded
 **/
static inline void kvioAddTraceRecord(KVIO *kvio, TraceLocation location)
{
  vioAddTraceRecord(kvio->vio, location);
}

/**
 * Set up the work item for a KVIO.
 *
 * @param kvio           The KVIO to set up
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
static inline void setupKVIOWork(KVIO             *kvio,
                                 KvdoWorkFunction  work,
                                 void             *statsFunction,
                                 unsigned int      action)
{
  setupWorkItem(&kvio->enqueueable.workItem, work, statsFunction, action);
}

/**
 * Set up and enqueue a KVIO.
 *
 * @param kvio           The KVIO to set up
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 * @param queue          The queue on which to enqueue the KVIO
 **/
static inline void launchKVIO(KVIO             *kvio,
                              KvdoWorkFunction  work,
                              void             *statsFunction,
                              unsigned int      action,
                              KvdoWorkQueue    *queue)
{
  setupKVIOWork(kvio, work, statsFunction, action);
  enqueueKVIOWork(queue, kvio);
}

/**
 * Move a KVIO back to the base threads.
 *
 * @param kvio The KVIO to enqueue
 **/
void kvdoEnqueueVIOCallback(KVIO *kvio);

/**
 * Handles kvio-related I/O post-processing.
 *
 * @param kvio  The kvio to finalize
 * @param error Possible error
 **/
void kvdoContinueKvio(KVIO *kvio, int error);

/**
 * Initialize a KVIO.
 *
 * @param kvio      The KVIO to initialize
 * @param layer     The physical layer
 * @param vioType   The type of VIO to create
 * @param priority  The relative priority to assign to the KVIO
 * @param parent    The parent of the KVIO completion
 * @param bio       The bio to associate with this KVIO
 **/
void initializeKVIO(KVIO        *kvio,
                    KernelLayer *layer,
                    VIOType      vioType,
                    VIOPriority  priority,
                    void        *parent,
                    BIO         *bio);

/**
 * Destroy a MetadataKVIO and NULL out the pointer to it.
 *
 * @param metadataKVIOPtr  A pointer to the MetadataKVIO to destroy
 **/
void freeMetadataKVIO(MetadataKVIO **metadataKVIOPtr);

/**
 * Destroy a CompressedWriteKVIO and NULL out the pointer to it.
 *
 * @param compressedWriteKVIOPtr  A pointer to the CompressedWriteKVIO to
 *                                destroy
 **/
void freeCompressedWriteKVIO(CompressedWriteKVIO **compressedWriteKVIOPtr);

/**
 * Create a new VIO (and its enclosing KVIO) for metadata operations.
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
 * Create a new AllocatingVIO (and its enclosing KVIO) for compressed writes.
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
 * Submit a compressed block write.
 *
 * <p>Implements CompressedWriter.
 *
 * @param allocatingVIO  The AllocatingVIO for the compressed write
 **/
void kvdoWriteCompressedBlock(AllocatingVIO *allocatingVIO);

/**
 * Read or write a single metadata VIO.
 *
 * <p>Implements MetadataReader and MetadataWriter
 *
 * @param vio  The VIO to read or write
 **/
void kvdoSubmitMetadataVIO(VIO *vio);

/**
 * Issue an empty flush to the lower layer using the BIO in a metadata VIO.
 *
 * <p>Implements MetadataWriter.
 *
 * @param vio  The VIO to flush
 **/
void kvdoFlushVIO(VIO *vio);

#endif /* KVIO_H */
