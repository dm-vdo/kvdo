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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/kvdoFlush.c#2 $
 */

#include "kvdoFlush.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "threadConfig.h"

#include "bio.h"
#include "ioSubmitter.h"

/**
 * A specific (concrete) encapsulation of flush requests.
 *
 * <p>We attempt to allocate a KVDOFlush objects for each incoming flush bio.
 * In case the allocate fails, a spare object is pre-allocated by and stored
 * in the kernel layer. The first time an allocation fails, the spare is used.
 * If another allocation fails while the spare is in use, it will merely be
 * queued for later processing.
 *
 * <p>When a KVDOFlush is complete, it will either be freed, immediately
 * re-used for queued flushes, or stashed in the kernel layer as the new spare
 * object. This ensures that we will always make forward progress.
 **/
struct kvdoFlush {
  KvdoWorkItem     workItem;
  KernelLayer     *layer;
  struct bio_list  bios;
  Jiffies          arrivalTime;  // Time when earliest bio appeared
  VDOFlush         vdoFlush;
};

/**********************************************************************/
int makeKVDOFlush(KVDOFlush **flushPtr)
{
  return ALLOCATE(1, KVDOFlush, __func__, flushPtr);
}

/**********************************************************************/
bool shouldProcessFlush(KernelLayer *layer)
{
  return (getKVDOWritePolicy(&layer->kvdo) == WRITE_POLICY_ASYNC);
}

/**
 * Function call to handle an empty flush request from the request queue.
 *
 * @param item  The work item representing the flush request
 **/
static void kvdoFlushWork(KvdoWorkItem *item)
{
  KVDOFlush *kvdoFlush = container_of(item, KVDOFlush, workItem);
  flush(kvdoFlush->layer->kvdo.vdo, &kvdoFlush->vdoFlush);
}

/**
 * Initialize a KVDOFlush object, transferring all the bios in the kernel
 * layer's waitingFlushes list to it. The caller MUST already hold the layer's
 * flushLock.
 *
 * @param kvdoFlush  The flush to initialize
 * @param layer      The kernel layer on which the flushLock is held
 **/
static void initializeKVDOFlush(KVDOFlush *kvdoFlush, KernelLayer *layer)
{
  kvdoFlush->layer = layer;
  bio_list_init(&kvdoFlush->bios);
  bio_list_merge(&kvdoFlush->bios, &layer->waitingFlushes);
  bio_list_init(&layer->waitingFlushes);
  kvdoFlush->arrivalTime = layer->flushArrivalTime;
}

/**********************************************************************/
static void enqueueKVDOFlush(KVDOFlush *kvdoFlush)
{
  setupWorkItem(&kvdoFlush->workItem, kvdoFlushWork, NULL, REQ_Q_ACTION_FLUSH);
  KVDO *kvdo = &kvdoFlush->layer->kvdo;
  enqueueKVDOWork(kvdo, &kvdoFlush->workItem,
                  getPackerZoneThread(getThreadConfig(kvdo->vdo)));
}

/**********************************************************************/
void launchKVDOFlush(KernelLayer *layer, BIO *bio)
{
  // Try to allocate a KVDOFlush to represent the flush request. If the
  // allocation fails, we'll deal with it later.
  KVDOFlush *kvdoFlush = kzalloc(sizeof(KVDOFlush), GFP_NOWAIT);

  spin_lock(&layer->flushLock);

  // We have a new bio to start.  Add it to the list.  If it becomes the
  // only entry on the list, record the time.
  if (bio_list_empty(&layer->waitingFlushes)) {
    layer->flushArrivalTime = jiffies;
  }
  bio_list_add(&layer->waitingFlushes, bio);

  if (kvdoFlush == NULL) {
    // The KVDOFlush allocation failed. Try to use the spare KVDOFlush object.
    if (layer->spareKVDOFlush == NULL) {
      // The spare is already in use. This bio is on waitingFlushes and it
      // will be handled by a flush completion or by a bio that can allocate.
      spin_unlock(&layer->flushLock);
      return;
    }

    // Take and use the spare KVDOFlush object.
    kvdoFlush = layer->spareKVDOFlush;
    layer->spareKVDOFlush = NULL;
  }

  // We have flushes to start. Capture them in the KVDOFlush object.
  initializeKVDOFlush(kvdoFlush, layer);

  spin_unlock(&layer->flushLock);

  // Finish launching the flushes.
  enqueueKVDOFlush(kvdoFlush);
}

/**
 * Release a KVDOFlush object that has completed its work. If there are any
 * pending flush requests whose KVDOFlush allocation failed, they will be
 * launched by immediately re-using the released KVDOFlush. If there is no
 * spare KVDOFlush, the released object will become the spare. Otherwise, the
 * KVDOFlush will be freed.
 *
 * @param kvdoFlush  The completed flush object to re-use or free
 **/
static void releaseKVDOFlush(KVDOFlush *kvdoFlush)
{
  KernelLayer *layer = kvdoFlush->layer;
  bool relaunchFlush = false;
  bool freeFlush     = false;

  spin_lock(&layer->flushLock);
  if (bio_list_empty(&layer->waitingFlushes)) {
    // Nothing needs to be started.  Save one spare KVDOFlush object.
    if (layer->spareKVDOFlush == NULL) {
      // Make the new spare all zero, just like a newly allocated one.
      memset(kvdoFlush, 0, sizeof(*kvdoFlush));
      layer->spareKVDOFlush = kvdoFlush;
    } else {
      freeFlush = true;
    }
  } else {
    // We have flushes to start.  Capture them in the KVDOFlush object.
    initializeKVDOFlush(kvdoFlush, layer);
    relaunchFlush = true;
  }
  spin_unlock(&layer->flushLock);

  if (relaunchFlush) {
    // Finish launching the flushes.
    enqueueKVDOFlush(kvdoFlush);
  } else if (freeFlush) {
    kfree(kvdoFlush);
  }
}

/**
 * Function called to complete and free a flush request
 *
 * @param item    The flush-request work item
 **/
static void kvdoCompleteFlushWork(KvdoWorkItem *item)
{
  KVDOFlush   *kvdoFlush = container_of(item, KVDOFlush, workItem);
  KernelLayer *layer     = kvdoFlush->layer;

  BIO *bio;
  while ((bio = bio_list_pop(&kvdoFlush->bios)) != NULL) {
    // We're not acknowledging this bio now, but we'll never touch it
    // again, so this is the last chance to account for it.
    countBios(&layer->biosAcknowledged, bio);

    // Make sure the bio is a empty flush bio.
    prepareFlushBIO(bio, bio->bi_private, layer->dev->bdev, bio->bi_end_io);
    atomic64_inc(&layer->flushOut);
    generic_make_request(bio);
  }


  // Release the KVDOFlush object, freeing it, re-using it as the spare, or
  // using it to launch any flushes that had to wait when allocations failed.
  releaseKVDOFlush(kvdoFlush);
}

/**********************************************************************/
void kvdoCompleteFlush(VDOFlush **kfp)
{
  if (*kfp != NULL) {
    KVDOFlush *kvdoFlush = container_of(*kfp, KVDOFlush, vdoFlush);
    setupWorkItem(&kvdoFlush->workItem, kvdoCompleteFlushWork, NULL,
                  BIO_Q_ACTION_FLUSH);
    enqueueBioWorkItem(kvdoFlush->layer->ioSubmitter,
                       &kvdoFlush->workItem);
    *kfp = NULL;
  }
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Callback for a synchronous flush bio.
 *
 * @param bio  The flush bio
 **/
static void endSynchronousFlush(BIO *bio)
#else
/**
 * Callback for a synchronous flush bio.
 *
 * @param bio  The flush bio
 * @param result  The result of the flush operation
 **/
static void endSynchronousFlush(BIO *bio, int result)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  int result = getBioResult(bio);
#endif

  if (result != 0) {
    logErrorWithStringError(result, "synchronous flush failed");
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,0)
    clear_bit(BIO_UPTODATE, &bio->bi_flags);
#endif
  }

  KernelLayer *layer = bio->bi_private;
  atomic64_inc(&layer->flushOut);
  complete(&layer->flushWait);
}

/**********************************************************************/
int synchronousFlush(KernelLayer *layer)
{
  BIO *bio;
  int result = createBio(layer, NULL, &bio);
  if (result != VDO_SUCCESS) {
    return result;
  }

  init_completion(&layer->flushWait);
  prepareFlushBIO(bio, layer, layer->dev->bdev, endSynchronousFlush);
  bio->bi_next = NULL;
  generic_make_request(bio);
  wait_for_completion(&layer->flushWait);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  if (getBioResult(bio) != 0) {
#else
  if (!bio_flagged(bio, BIO_UPTODATE)) {
#endif
    result = -EIO;
  }

  freeBio(bio, layer);
  return result;
}
