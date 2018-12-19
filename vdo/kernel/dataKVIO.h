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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dataKVIO.h#3 $
 */

#ifndef DATA_KVIO_H
#define DATA_KVIO_H

#include "dataVIO.h"
#include "kvio.h"
#include "uds-block.h"

struct external_io_request {
  /*
   * The BIO which was received from the device mapper to initiate an I/O
   * request. This field will be non-NULL only until the request is
   * acknowledged.
   */
  BIO           *bio;
  // Cached copies of fields from the bio which will need to be reset after
  // we're done.
  void          *private;
  void          *endIO;
  // This is a copy of the bi_rw field of the BIO which sadly is not just
  // a boolean read-write flag, but also includes other flag bits.
  unsigned long  rw;
};

/* Dedupe support */
struct dedupeContext {
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
  BIO                 *bio;
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

struct dataKVIO {
  /* The embedded base code's DataVIO */
  DataVIO                     dataVIO;
  /* The embedded KVIO */
  KVIO                        kvio;
  /* The BIO from the request which is being serviced by this KVIO. */
  struct external_io_request  externalIORequest;
  /* Dedupe */
  DedupeContext               dedupeContext;
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
  BIO               *dataBlockBio;
  /** A block used as output during compression or uncompression. */
  char              *scratchBlock;
};

/**
 * Convert a KVIO to a DataKVIO.
 *
 * @param kvio  The KVIO to convert
 *
 * @return The KVIO as a DataKVIO
 **/
static inline DataKVIO *kvioAsDataKVIO(KVIO *kvio)
{
  ASSERT_LOG_ONLY(isData(kvio), "KVIO is a DataKVIO");
  return container_of(kvio, DataKVIO, kvio);
}

/**
 * Convert a DataKVIO to a KVIO.
 *
 * @param dataKVIO  The DataKVIO to convert
 *
 * @return The DataKVIO as a KVIO
 **/
static inline KVIO *dataKVIOAsKVIO(DataKVIO *dataKVIO)
{
  return &dataKVIO->kvio;
}

/**
 * Returns a pointer to the DataKVIO wrapping a DataVIO.
 *
 * @param dataVIO  the DataVIO
 *
 * @return the DataKVIO
 **/
static inline DataKVIO *dataVIOAsDataKVIO(DataVIO *dataVIO)
{
  return container_of(dataVIO, DataKVIO, dataVIO);
}

/**
 * Returns a pointer to the KVIO associated with a DataVIO.
 *
 * @param dataVIO  the DataVIO
 *
 * @return the KVIO
 **/
static inline KVIO *dataVIOAsKVIO(DataVIO *dataVIO)
{
  return dataKVIOAsKVIO(dataVIOAsDataKVIO(dataVIO));
}

/**
 * Returns a pointer to the DataKVIO wrapping a work item.
 *
 * @param item  the work item
 *
 * @return the DataKVIO
 **/
static inline DataKVIO *workItemAsDataKVIO(KvdoWorkItem *item)
{
  return kvioAsDataKVIO(workItemAsKVIO(item));
}

/**
 * Get the WorkItem from a DataKVIO.
 *
 * @param dataKVIO  The DataKVIO
 *
 * @return the DataKVIO's work item
 **/
static inline KvdoWorkItem *workItemFromDataKVIO(DataKVIO *dataKVIO)
{
  return &dataKVIOAsKVIO(dataKVIO)->enqueueable.workItem;
}

/**
 * Get the BIO from a DataKVIO.
 *
 * @param dataKVIO  The DataKVIO from which to get the BIO
 *
 * @return The DataKVIO's BIO
 **/
static inline BIO *getBIOFromDataKVIO(DataKVIO *dataKVIO)
{
  return dataKVIOAsKVIO(dataKVIO)->bio;
}

/**
 * Get the KernelLayer from a DataKVIO.
 *
 * @param dataKVIO  The DataKVIO from which to get the KernelLayer
 *
 * @return The DataKVIO's KernelLayer
 **/
static inline KernelLayer *getLayerFromDataKVIO(DataKVIO *dataKVIO)
{
  return dataKVIOAsKVIO(dataKVIO)->layer;
}

/**
 * Set up and enqueue a DataKVIO's work item to be processed in the base code
 * context.
 *
 * @param dataKVIO       The DataKVIO with the work item to be run
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
static inline void enqueueDataKVIO(DataKVIO         *dataKVIO,
                                   KvdoWorkFunction  work,
                                   void             *statsFunction,
                                   unsigned int      action)
{
  enqueueKVIO(dataKVIOAsKVIO(dataKVIO), work, statsFunction, action);
}

/**
 * Enqueue a DataKVIO on a work queue.
 *
 * @param queue     The queue
 * @param dataKVIO  The DataKVIO
 **/
static inline void enqueueDataKVIOWork(KvdoWorkQueue *queue,
                                       DataKVIO      *dataKVIO)
{
  enqueueKVIOWork(queue, dataKVIOAsKVIO(dataKVIO));
}

/**
 * Add a trace record for the current source location.
 *
 * @param dataKVIO  The DataKVIO structure to be updated
 * @param location  The source-location descriptor to be recorded
 **/
static inline void dataKVIOAddTraceRecord(DataKVIO      *dataKVIO,
                                          TraceLocation  location)
{
  dataVIOAddTraceRecord(&dataKVIO->dataVIO, location);
}

/**
 * Set up and enqueue a DataKVIO on the CPU queue.
 *
 * @param dataKVIO       The DataKVIO to set up
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
static inline void launchDataKVIOOnCPUQueue(DataKVIO         *dataKVIO,
                                            KvdoWorkFunction  work,
                                            void             *statsFunction,
                                            unsigned int      action)
{
  KVIO *kvio = dataKVIOAsKVIO(dataKVIO);
  launchKVIO(kvio, work, statsFunction, action, kvio->layer->cpuQueue);
}

/**
 * Set up and enqueue a DataKVIO on the BIO Ack queue.
 *
 * @param dataKVIO       The DataKVIO to set up
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
static inline void launchDataKVIOOnBIOAckQueue(DataKVIO         *dataKVIO,
                                               KvdoWorkFunction  work,
                                               void             *statsFunction,
                                               unsigned int      action)
{
  KVIO *kvio = dataKVIOAsKVIO(dataKVIO);
  launchKVIO(kvio, work, statsFunction, action, kvio->layer->bioAckQueue);
}

/**
 * Move a DataKVIO back to the base threads.
 *
 * @param dataKVIO The DataKVIO to enqueue
 **/
static inline void kvdoEnqueueDataVIOCallback(DataKVIO *dataKVIO)
{
  kvdoEnqueueVIOCallback(dataKVIOAsKVIO(dataKVIO));
}

/**
 * Check whether the external request bio had FUA set.
 *
 * @param dataKVIO  The DataKVIO to check
 *
 * @return <code>true</code> if the external request bio had FUA set
 **/
static inline bool requestorSetFUA(DataKVIO *dataKVIO)
{
  return ((dataKVIO->externalIORequest.rw & REQ_FUA) == REQ_FUA);
}

/**
 * Associate a KVIO with a BIO passed in from the block layer, and start
 * processing the KVIO.
 *
 * If setting up a KVIO fails, a message is logged, and the limiter permits
 * (request and maybe discard) released, but the caller is responsible for
 * disposing of the bio.
 *
 * @param layer                 The physical layer
 * @param bio                   The bio for which to create KVIO
 * @param arrivalTime           The time (in jiffies) when the external request
 *                              entered the device mapbio function
 * @param hasDiscardPermit      Whether we got a permit from the discardLimiter
 *                              of the kernel layer
 *
 * @return VDO_SUCCESS or a system error code
 **/
int kvdoLaunchDataKVIOFromBio(KernelLayer *layer,
                              BIO         *bio,
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
 * Implements DataVIOZeroer.
 *
 * @param dataVIO  The DataVIO to zero
 **/
void kvdoZeroDataVIO(DataVIO *dataVIO);

/**
 * Implements DataCopier.
 *
 * @param source       The DataVIO to copy from
 * @param destination  The DataVIO to copy to
 **/
void kvdoCopyDataVIO(DataVIO *source, DataVIO *destination);

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
 * Implements DataReader.
 *
 * @param dataVIO  The DataVIO to read
 **/
void kvdoReadDataVIO(DataVIO *dataVIO);

/**
 * Implements DataWriter.
 *
 * @param dataVIO  The DataVIO to write
 **/
void kvdoWriteDataVIO(DataVIO *dataVIO);

/**
 * Implements DataModifier.
 *
 * @param dataVIO  The DataVIO to modify
 **/
void kvdoModifyWriteDataVIO(DataVIO *dataVIO);

/**
 * Implements DataHasher.
 *
 * @param dataVIO  The DataVIO to hash
 **/
void kvdoHashDataVIO(DataVIO *dataVIO);

/**
 * Implements DuplicationChecker.
 *
 * @param dataVIO  The DataVIO containing the block to check
 **/
void kvdoCheckForDuplication(DataVIO *dataVIO);

/**
 * Implements DataAcknowledger.
 *
 * @param dataVIO  The DataVIO to acknowledge
 **/
void kvdoAcknowledgeDataVIO(DataVIO *dataVIO);

/**
 * Implements DataCompressor.
 *
 * @param dataVIO  The DataVIO to compress
 **/
void kvdoCompressDataVIO(DataVIO *dataVIO);

/**
 * Implements AlbireoUpdater.
 *
 * @param dataVIO  The DataVIO which needs to change the entry for its data
 **/
void kvdoUpdateDedupeAdvice(DataVIO *dataVIO);

/**
 * Allocate a buffer pool of DataKVIOs.
 *
 * @param [in]  layer          The layer in which the DataKVIOs will operate
 * @param [in]  poolSize       The number of DataKVIOs in the pool
 * @param [out] bufferPoolPtr  A pointer to hold the new buffer pool
 *
 * @return VDO_SUCCESS or an error
 **/
int makeDataKVIOBufferPool(KernelLayer  *layer,
                           uint32_t      poolSize,
                           BufferPool  **bufferPoolPtr)
  __attribute__((warn_unused_result));

/**
 * Get the state needed to generate UDS metadata from the DataKVIO
 * associated with a DedupeContext.
 *
 * @param context  The DedupeContext
 *
 * @return the advice to store in the UDS index
 **/
DataLocation getDedupeAdvice(const DedupeContext *context)
  __attribute__((warn_unused_result));

/**
 * Set the result of a dedupe query for the DataKVIO associated with a
 * DedupeContext.
 *
 * @param context  The context receiving advice
 * @param advice   A data location at which the chunk named in the context
 *                 might be stored (will be NULL if no advice was found)
 **/
void setDedupeAdvice(DedupeContext *context, const DataLocation *advice);

#endif /* DATA_KVIO_H */
