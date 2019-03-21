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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dataKVIO.h#15 $
 */

#ifndef DATA_KVIO_H
#define DATA_KVIO_H

#include "dataVIO.h"
#include "kvio.h"
#include "uds-block.h"

struct external_io_request {
  /*
   * The bio which was received from the device mapper to initiate an I/O
   * request. This field will be non-NULL only until the request is
   * acknowledged.
   */
  struct bio    *bio;
  // Cached copies of fields from the bio which will need to be reset after
  // we're done.
  void          *private;
  void          *endIO;
  // This is a copy of the bi_rw field of the bio which sadly is not just
  // a boolean read-write flag, but also includes other flag bits.
  unsigned long  rw;
};

/* Dedupe support */
struct dedupe_context {
  UdsRequest          udsRequest;
  struct list_head    pendingList;
  Jiffies             submissionTime;
  Atomic32            requestState;
  int                 status;
  bool                isPending;
  /** Hash of the associated VIO (NULL if not calculated) */
  const UdsChunkName *chunkName;
};

struct read_block {
  /**
   * A pointer to a block that holds the data from the last read operation.
   **/
  char                *data;
  /**
   * Temporary storage for doing reads from the underlying device.
   **/
  char                *buffer;
  /**
   * A bio structure wrapping the buffer.
   **/
  struct bio          *bio;
  /**
   * Callback to invoke after completing the read I/O operation.
   **/
  DataKVIOCallback     callback;
  /**
   * Mapping state passed to kvdoReadBlock(), used to determine whether
   * the data must be uncompressed.
   **/
  BlockMappingState    mappingState;
  /**
   * The result code of the read attempt.
   **/
  int                  status;
};

struct data_kvio {
  /* The embedded base code's DataVIO */
  DataVIO                     dataVIO;
  /* The embedded kvio */
  struct kvio                 kvio;
  /* The bio from the request which is being serviced by this kvio. */
  struct external_io_request  externalIORequest;
  /* Dedupe */
  struct dedupe_context       dedupeContext;
  /* Read cache */
  struct read_block           readBlock;
  /* partial block support */
  BlockSize                   offset;
  bool                        isPartial;
  /* discard support */
  bool                        hasDiscardPermit;
  DiscardSize                 remainingDiscard;
  /**
   * A copy of user data written, so we can do additional processing
   * (dedupe, compression) after acknowledging the I/O operation and
   * thus losing access to the original data.
   *
   * Also used as buffer space for read-modify-write cycles when
   * emulating smaller-than-blockSize I/O operations.
   **/
  char              *dataBlock;
  /** A bio structure describing the #dataBlock buffer. */
  struct bio        *dataBlockBio;
  /** A block used as output during compression or uncompression. */
  char              *scratchBlock;
};

/**
 * Convert a kvio to a data_kvio.
 *
 * @param kvio  The kvio to convert
 *
 * @return The kvio as a data_kvio
 **/
static inline struct data_kvio *kvioAsDataKVIO(struct kvio *kvio)
{
  ASSERT_LOG_ONLY(isData(kvio), "kvio is a data_kvio");
  return container_of(kvio, struct data_kvio, kvio);
}

/**
 * Convert a data_kvio to a kvio.
 *
 * @param dataKVIO  The data_kvio to convert
 *
 * @return The data_kvio as a kvio
 **/
static inline struct kvio *dataKVIOAsKVIO(struct data_kvio *dataKVIO)
{
  return &dataKVIO->kvio;
}

/**
 * Returns a pointer to the data_kvio wrapping a DataVIO.
 *
 * @param dataVIO  the DataVIO
 *
 * @return the data_kvio
 **/
static inline struct data_kvio *dataVIOAsDataKVIO(DataVIO *dataVIO)
{
  return container_of(dataVIO, struct data_kvio, dataVIO);
}

/**
 * Returns a pointer to the kvio associated with a DataVIO.
 *
 * @param dataVIO  the DataVIO
 *
 * @return the kvio
 **/
static inline struct kvio *dataVIOAsKVIO(DataVIO *dataVIO)
{
  return dataKVIOAsKVIO(dataVIOAsDataKVIO(dataVIO));
}

/**
 * Returns a pointer to the data_kvio wrapping a work item.
 *
 * @param item  the work item
 *
 * @return the data_kvio
 **/
static inline struct data_kvio *workItemAsDataKVIO(struct kvdo_work_item *item)
{
  return kvioAsDataKVIO(workItemAsKVIO(item));
}

/**
 * Get the WorkItem from a data_kvio.
 *
 * @param dataKVIO  The data_kvio
 *
 * @return the data_kvio's work item
 **/
static inline struct kvdo_work_item *
workItemFromDataKVIO(struct data_kvio *dataKVIO)
{
  return &dataKVIOAsKVIO(dataKVIO)->enqueueable.workItem;
}

/**
 * Get the bio from a data_kvio.
 *
 * @param dataKVIO  The data_kvio from which to get the bio
 *
 * @return The data_kvio's bio
 **/
static inline struct bio *getBIOFromDataKVIO(struct data_kvio *dataKVIO)
{
  return dataKVIOAsKVIO(dataKVIO)->bio;
}

/**
 * Get the KernelLayer from a data_kvio.
 *
 * @param dataKVIO  The data_kvio from which to get the KernelLayer
 *
 * @return The data_kvio's KernelLayer
 **/
static inline KernelLayer *getLayerFromDataKVIO(struct data_kvio *dataKVIO)
{
  return dataKVIOAsKVIO(dataKVIO)->layer;
}

/**
 * Set up and enqueue a data_kvio's work item to be processed in the base code
 * context.
 *
 * @param dataKVIO       The data_kvio with the work item to be run
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
static inline void enqueueDataKVIO(struct data_kvio *dataKVIO,
                                   KvdoWorkFunction  work,
                                   void             *statsFunction,
                                   unsigned int      action)
{
  enqueueKVIO(dataKVIOAsKVIO(dataKVIO), work, statsFunction, action);
}

/**
 * Enqueue a data_kvio on a work queue.
 *
 * @param queue     The queue
 * @param dataKVIO  The data_kvio
 **/
static inline void enqueueDataKVIOWork(struct kvdo_work_queue *queue,
                                       struct data_kvio       *dataKVIO)
{
  enqueueKVIOWork(queue, dataKVIOAsKVIO(dataKVIO));
}

/**
 * Add a trace record for the current source location.
 *
 * @param dataKVIO  The data_kvio structure to be updated
 * @param location  The source-location descriptor to be recorded
 **/
static inline void dataKVIOAddTraceRecord(struct data_kvio *dataKVIO,
                                          TraceLocation     location)
{
  dataVIOAddTraceRecord(&dataKVIO->dataVIO, location);
}

/**
 * Set up and enqueue a data_kvio on the CPU queue.
 *
 * @param dataKVIO       The data_kvio to set up
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
static inline void launchDataKVIOOnCPUQueue(struct data_kvio *dataKVIO,
                                            KvdoWorkFunction  work,
                                            void             *statsFunction,
                                            unsigned int      action)
{
  struct kvio *kvio = dataKVIOAsKVIO(dataKVIO);
  launchKVIO(kvio, work, statsFunction, action, kvio->layer->cpuQueue);
}

/**
 * Set up and enqueue a data_kvio on the bio Ack queue.
 *
 * @param dataKVIO       The data_kvio to set up
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
static inline void launchDataKVIOOnBIOAckQueue(struct data_kvio *dataKVIO,
                                               KvdoWorkFunction  work,
                                               void             *statsFunction,
                                               unsigned int      action)
{
  struct kvio *kvio = dataKVIOAsKVIO(dataKVIO);
  launchKVIO(kvio, work, statsFunction, action, kvio->layer->bioAckQueue);
}

/**
 * Move a data_kvio back to the base threads.
 *
 * @param dataKVIO The data_kvio to enqueue
 **/
static inline void kvdoEnqueueDataVIOCallback(struct data_kvio *dataKVIO)
{
  kvdoEnqueueVIOCallback(dataKVIOAsKVIO(dataKVIO));
}

/**
 * Check whether the external request bio had FUA set.
 *
 * @param dataKVIO  The data_kvio to check
 *
 * @return <code>true</code> if the external request bio had FUA set
 **/
static inline bool requestorSetFUA(struct data_kvio *dataKVIO)
{
  return ((dataKVIO->externalIORequest.rw & REQ_FUA) == REQ_FUA);
}

/**
 * Associate a kvio with a bio passed in from the block layer, and start
 * processing the kvio.
 *
 * If setting up a kvio fails, a message is logged, and the limiter permits
 * (request and maybe discard) released, but the caller is responsible for
 * disposing of the bio.
 *
 * @param layer                 The physical layer
 * @param bio                   The bio for which to create kvio
 * @param arrivalTime           The time (in jiffies) when the external request
 *                              entered the device mapbio function
 * @param hasDiscardPermit      Whether we got a permit from the discardLimiter
 *                              of the kernel layer
 *
 * @return VDO_SUCCESS or a system error code
 **/
int kvdoLaunchDataKVIOFromBio(KernelLayer *layer,
                              struct bio  *bio,
                              Jiffies      arrivalTime,
                              bool         hasDiscardPermit)
  __attribute__((warn_unused_result));

/**
 * Return a batch of DataKVIOs to the pool.
 *
 * <p>Implements BatchProcessorCallback.
 *
 * @param batch    The batch processor
 * @param closure  The kernal layer
 **/
void returnDataKVIOBatchToPool(struct batch_processor *batch, void *closure);

/**
 * Fetch the data for a block from storage. The fetched data will be
 * uncompressed when the callback is called, and the result of the read
 * operation will be stored in the read_block's status field. On success,
 * the data will be in the read_block's data pointer.
 *
 * @param dataVIO       The DataVIO to read a block in for
 * @param location      The physical block number to read from
 * @param mappingState  The mapping state of the block to read
 * @param action        The bio queue action
 * @param callback      The function to call when the read is done
 **/
void kvdoReadBlock(DataVIO             *dataVIO,
                   PhysicalBlockNumber  location,
                   BlockMappingState    mappingState,
                   BioQAction           action,
                   DataKVIOCallback     callback);

/**
 * Allocate a buffer pool of DataKVIOs.
 *
 * @param [in]  layer          The layer in which the DataKVIOs will operate
 * @param [in]  poolSize       The number of DataKVIOs in the pool
 * @param [out] bufferPoolPtr  A pointer to hold the new buffer pool
 *
 * @return VDO_SUCCESS or an error
 **/
int makeDataKVIOBufferPool(KernelLayer          *layer,
                           uint32_t              poolSize,
                           struct buffer_pool  **bufferPoolPtr)
  __attribute__((warn_unused_result));

/**
 * Get the state needed to generate UDS metadata from the data_kvio
 * associated with a dedupe_context.
 *
 * @param context  The dedupe_context
 *
 * @return the advice to store in the UDS index
 **/
DataLocation getDedupeAdvice(const struct dedupe_context *context)
  __attribute__((warn_unused_result));

/**
 * Set the result of a dedupe query for the data_kvio associated with a
 * dedupe_context.
 *
 * @param context  The context receiving advice
 * @param advice   A data location at which the chunk named in the context
 *                 might be stored (will be NULL if no advice was found)
 **/
void setDedupeAdvice(struct dedupe_context *context,
                     const DataLocation *advice);

#endif /* DATA_KVIO_H */
