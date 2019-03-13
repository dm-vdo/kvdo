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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvio.c#10 $
 */

#include "kvio.h"


#include "logger.h"
#include "memoryAlloc.h"

#include "numUtils.h"
#include "vdo.h"
#include "waitQueue.h"

#include "bio.h"
#include "ioSubmitter.h"
#include "kvdoFlush.h"

/**
 * A function to tell vdo that we have completed the requested async
 * operation for a vio
 *
 * @param item    The work item of the VIO to complete
 **/
static void kvdoHandleVIOCallback(KvdoWorkItem *item)
{
  KVIO *kvio = workItemAsKVIO(item);
  runCallback(vioAsCompletion(kvio->vio));
}

/**********************************************************************/
void kvdoEnqueueVIOCallback(KVIO *kvio)
{
  enqueueKVIO(kvio, kvdoHandleVIOCallback,
              (KvdoWorkFunction) vioAsCompletion(kvio->vio)->callback,
              REQ_Q_ACTION_VIO_CALLBACK);
}

/**********************************************************************/
void kvdoContinueKvio(KVIO *kvio, int error)
{
  if (unlikely(error != VDO_SUCCESS)) {
    setCompletionResult(vioAsCompletion(kvio->vio), error);
  }
  kvdoEnqueueVIOCallback(kvio);
}

/**********************************************************************/
// noinline ensures systemtap can hook in here
static noinline void maybeLogKvioTrace(KVIO *kvio)
{
  if (kvio->layer->traceLogging) {
    logKvioTrace(kvio);
  }
}

/**********************************************************************/
static void freeKVIO(KVIO **kvioPtr)
{
  KVIO *kvio = *kvioPtr;
  if (kvio == NULL) {
    return;
  }

  if (unlikely(kvio->vio->trace != NULL)) {
    maybeLogKvioTrace(kvio);
    FREE(kvio->vio->trace);
  }

  free_bio(kvio->bio, kvio->layer);
  FREE(kvio);
  *kvioPtr = NULL;
}

/**********************************************************************/
void freeMetadataKVIO(struct metadata_kvio **metadataKVIOPtr)
{
  freeKVIO((KVIO **) metadataKVIOPtr);
}

/**********************************************************************/
void freeCompressedWriteKVIO(struct compressed_write_kvio **compressedWriteKVIOPtr)
{
  freeKVIO((KVIO **) compressedWriteKVIOPtr);
}

/**********************************************************************/
void writeCompressedBlock(AllocatingVIO *allocatingVIO)
{
  // This method assumes that compressed writes never set the flush or FUA
  // bits.
  struct compressed_write_kvio *compressedWriteKVIO
    = allocatingVIOAsCompressedWriteKVIO(allocatingVIO);
  KVIO *kvio       = compressedWriteKVIOAsKVIO(compressedWriteKVIO);
  struct bio *bio  = kvio->bio;
  reset_bio(bio, kvio->layer);
  set_bio_operation_write(bio);
  set_bio_sector(bio, blockToSector(kvio->layer, kvio->vio->physical));
  vdo_submit_bio(bio, BIO_Q_ACTION_COMPRESSED_DATA);
}

/**
 * Get the BioQueue action for a metadata VIO based on that VIO's priority.
 *
 * @param vio  The VIO
 *
 * @return The action with which to submit the VIO's bio.
 **/
static inline BioQAction getMetadataAction(VIO *vio)
{
  return ((vio->priority == VIO_PRIORITY_HIGH)
          ? BIO_Q_ACTION_HIGH : BIO_Q_ACTION_METADATA);
}

/**********************************************************************/
void submitMetadataVIO(VIO *vio)
{
  KVIO       *kvio = metadataKVIOAsKVIO(vioAsMetadataKVIO(vio));
  struct bio *bio  = kvio->bio;
  reset_bio(bio, kvio->layer);

  set_bio_sector(bio, blockToSector(kvio->layer, vio->physical));

  // Metadata I/Os bypass the read cache.
  if (isReadVIO(vio)) {
    ASSERT_LOG_ONLY(!vioRequiresFlushBefore(vio),
                    "read VIO does not require flush before");
    vioAddTraceRecord(vio, THIS_LOCATION("$F;io=readMeta"));
    set_bio_operation_read(bio);
  } else if (vioRequiresFlushBefore(vio)) {
    set_bio_operation_write(bio);
    set_bio_operation_flag_preflush(bio);
    vioAddTraceRecord(vio, THIS_LOCATION("$F;io=flushWriteMeta"));
  } else {
    set_bio_operation_write(bio);
    vioAddTraceRecord(vio, THIS_LOCATION("$F;io=writeMeta"));
  }

  if (vioRequiresFlushAfter(vio)) {
    set_bio_operation_flag_fua(bio);
  }
  vdo_submit_bio(bio, getMetadataAction(vio));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Handle the completion of a base-code initiated flush by continuing the flush
 * VIO.
 *
 * @param bio    The bio to complete
 **/
static void completeFlushBio(struct bio *bio)
#else
/**
 * Handle the completion of a base-code initiated flush by continuing the flush
 * VIO.
 *
 * @param bio    The bio to complete
 * @param error  Possible error from underlying block device
 **/
static void completeFlushBio(struct bio *bio, int error)
#endif
{
  KVIO *kvio   = (KVIO *) bio->bi_private;
  // Restore the bio's notion of its own data.
  reset_bio(bio, kvio->layer);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  kvdoContinueKvio(kvio, get_bio_result(bio));
#else
  kvdoContinueKvio(kvio, error);
#endif
}

/**********************************************************************/
void kvdoFlushVIO(VIO *vio)
{
  KVIO        *kvio  = metadataKVIOAsKVIO(vioAsMetadataKVIO(vio));
  struct bio  *bio   = kvio->bio;
  KernelLayer *layer = kvio->layer;
  reset_bio(bio, layer);
  prepare_flush_bio(bio, kvio, getKernelLayerBdev(layer), completeFlushBio);
  vdo_submit_bio(bio, getMetadataAction(vio));
}

/*
 * Hook for a SystemTap probe to potentially restrict the choices
 * of which VIOs should have their latencies tracked.
 *
 * Normally returns true. Even if true is returned, sampleThisOne may
 * cut down the monitored VIOs by some fraction so as to reduce the
 * impact on system performance.
 *
 * Must be "noinline" so that SystemTap can find the return
 * instruction and modify the return value.
 *
 * @param kvio   The KVIO being initialized
 * @param layer  The kernel layer
 * @param bio    The incoming I/O request
 *
 * @return whether it's useful to track latency for VIOs looking like
 *         this one
 */
static noinline bool
sampleThisVIO(KVIO *kvio, KernelLayer *layer, struct bio *bio)
{
  bool result = true;
  // Ensure the arguments and result exist at the same time, for SystemTap.
  __asm__ __volatile__(""
                       : "=g" (result)
                       : "0" (result),
                         "g" (kvio),
                         "g" (layer),
                         "g" (bio)
                       : "memory");
  return result;
}

/**********************************************************************/
void initializeKVIO(KVIO        *kvio,
                    KernelLayer *layer,
                    VIOType      vioType,
                    VIOPriority  priority,
                    void        *parent,
                    struct bio  *bio)
{
  if (layer->vioTraceRecording
      && sampleThisVIO(kvio, layer, bio)
      && sampleThisOne(&layer->traceSampleCounter)) {
    int result = (isDataVIOType(vioType)
                  ? allocTraceFromPool(layer, &kvio->vio->trace)
                  : ALLOCATE(1, Trace, "trace", &kvio->vio->trace));
    if (result != VDO_SUCCESS) {
      logError("trace record allocation failure %d", result);
    }
  }

  kvio->bio   = bio;
  kvio->layer = layer;
  if (bio != NULL) {
    bio->bi_private = kvio;
  }

  initializeVIO(kvio->vio, vioType, priority, parent, getVDO(&layer->kvdo),
                &layer->common);

  // XXX: The "init" label should be replaced depending on the
  // write/read/flush path followed.
  kvioAddTraceRecord(kvio, THIS_LOCATION("$F;io=?init;j=normal"));

  VDOCompletion *completion                = vioAsCompletion(kvio->vio);
  kvio->enqueueable.enqueueable.completion = completion;
  completion->enqueueable                  = &kvio->enqueueable.enqueueable;
}

/**
 * Construct a metadata KVIO.
 *
 * @param [in]  layer            The physical layer
 * @param [in]  vioType          The type of VIO to create
 * @param [in]  priority         The relative priority to assign to the
 *                               metadata_kvio
 * @param [in]  parent           The parent of the metadata_kvio completion
 * @param [in]  bio              The bio to associate with this metadata_kvio
 * @param [out] metadataKVIOPtr  A pointer to hold the new metadata_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int makeMetadataKVIO(KernelLayer           *layer,
                            VIOType                vioType,
                            VIOPriority            priority,
                            void                  *parent,
                            struct bio            *bio,
                            struct metadata_kvio **metadataKVIOPtr)
{
  // If struct metadata_kvio grows past 256 bytes, we'll lose benefits of
  // VDOSTORY-176.
  STATIC_ASSERT(sizeof(struct metadata_kvio) <= 256);

  // Metadata VIOs should use direct allocation and not use the buffer pool,
  // which is reserved for submissions from the linux block layer.
  struct metadata_kvio *metadataKVIO;
  int result = ALLOCATE(1, struct metadata_kvio, __func__, &metadataKVIO);
  if (result != VDO_SUCCESS) {
    logError("metadata KVIO allocation failure %d", result);
    return result;
  }

  KVIO *kvio = &metadataKVIO->kvio;
  kvio->vio  = &metadataKVIO->vio;
  initializeKVIO(kvio, layer, vioType, priority, parent, bio);
  *metadataKVIOPtr = metadataKVIO;
  return VDO_SUCCESS;
}

/**
 * Construct a struct compressed_write_kvio.
 *
 * @param [in]  layer                   The physical layer
 * @param [in]  parent                  The parent of the compressed_write_kvio
 *                                      completion
 * @param [in]  bio                     The bio to associate with this
 *                                      compressed_write_kvio
 * @param [out] compressedWriteKVIOPtr  A pointer to hold the new
 *                                      compressed_write_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int
makeCompressedWriteKVIO(KernelLayer                   *layer,
                        void                          *parent,
                        struct bio                    *bio,
                        struct compressed_write_kvio **compressedWriteKVIOPtr)
{
  // Compressed write VIOs should use direct allocation and not use the buffer
  // pool, which is reserved for submissions from the linux block layer.
  struct compressed_write_kvio *compressedWriteKVIO;
  int result = ALLOCATE(1, struct compressed_write_kvio, __func__,
                        &compressedWriteKVIO);
  if (result != VDO_SUCCESS) {
    logError("compressed write KVIO allocation failure %d", result);
    return result;
  }

  KVIO *kvio = &compressedWriteKVIO->kvio;
  kvio->vio  = allocatingVIOAsVIO(&compressedWriteKVIO->allocatingVIO);
  initializeKVIO(kvio, layer, VIO_TYPE_COMPRESSED_BLOCK,
                 VIO_PRIORITY_COMPRESSED_DATA, parent, bio);
  *compressedWriteKVIOPtr = compressedWriteKVIO;
  return VDO_SUCCESS;
}

/**********************************************************************/
int kvdoCreateMetadataVIO(PhysicalLayer  *layer,
                          VIOType         vioType,
                          VIOPriority     priority,
                          void           *parent,
                          char           *data,
                          VIO           **vioPtr)
{
  int result = ASSERT(isMetadataVIOType(vioType),
                      "%d is a metadata type", vioType);
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct bio *bio;
  KernelLayer *kernelLayer = asKernelLayer(layer);
  result = create_bio(kernelLayer, data, &bio);
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct metadata_kvio *metadataKVIO;
  result = makeMetadataKVIO(kernelLayer, vioType, priority, parent, bio,
                            &metadataKVIO);
  if (result != VDO_SUCCESS) {
    free_bio(bio, kernelLayer);
    return result;
  }

  *vioPtr = &metadataKVIO->vio;
  return VDO_SUCCESS;
}

/**********************************************************************/
int kvdoCreateCompressedWriteVIO(PhysicalLayer  *layer,
                                 void           *parent,
                                 char           *data,
                                 AllocatingVIO **allocatingVIOPtr)
{
  struct bio *bio;
  KernelLayer *kernelLayer = asKernelLayer(layer);
  int result = create_bio(kernelLayer, data, &bio);
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct compressed_write_kvio *compressedWriteKVIO;
  result = makeCompressedWriteKVIO(kernelLayer, parent, bio,
                                   &compressedWriteKVIO);
  if (result != VDO_SUCCESS) {
    free_bio(bio, kernelLayer);
    return result;
  }

  *allocatingVIOPtr = &compressedWriteKVIO->allocatingVIO;
  return VDO_SUCCESS;
}
