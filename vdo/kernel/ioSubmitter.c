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
 * $Id: //eng/vdo-releases/magnesium-rhel7.6/src/c++/vdo/kernel/ioSubmitter.c#1 $
 */

#include "ioSubmitterInternals.h"

#include "memoryAlloc.h"

#include "bio.h"
#include "kernelLayer.h"
#include "logger.h"
#include "readCache.h"

enum {
  /*
   * Whether to use bio merging code.
   *
   * Merging I/O requests in the request queue below us is helpful for
   * many devices, and VDO does a good job sometimes of shuffling up
   * the I/O order (too much for some simple I/O schedulers to sort
   * out) as we deal with dedupe advice etc. The bio map tracks the
   * yet-to-be-submitted I/O requests by block number so that we can
   * collect together and submit sequential I/O operations that should
   * be easy to merge. (So we don't actually *merge* them here, we
   * just arrange them so that merging can happen.)
   *
   * For some devices, merging may not help, and we may want to turn
   * off this code and save compute/spinlock cycles.
   */
  USE_BIOMAP           = 1,
};

/**
 * Increments appropriate counters for bio completions
 *
 * @param kvio the kvio associated with the bio
 * @param bio  the bio to count
 */
static void countAllBiosCompleted(KVIO *kvio, BIO *bio)
{
  KernelLayer *layer = kvio->layer;
  if (isData(kvio)) {
    countBios(&layer->biosOutCompleted, bio);
    return;
  }

  countBios(&layer->biosMetaCompleted, bio);
  if (kvio->vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
    countBios(&layer->biosJournalCompleted, bio);
  } else if (kvio->vio->type == VIO_TYPE_BLOCK_MAP) {
    countBios(&layer->biosPageCacheCompleted, bio);
  }
}

/**********************************************************************/
void countCompletedBios(BIO *bio)
{
  KVIO        *kvio  = (KVIO *)bio->bi_private;
  KernelLayer *layer = kvio->layer;
  atomic64_inc(&layer->biosCompleted);
  countAllBiosCompleted(kvio, bio);
}

/**********************************************************************/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
void completeAsyncBio(BIO *bio)
#else
void completeAsyncBio(BIO *bio, int error)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  int error = getBioResult(bio);
#endif
  KVIO *kvio = (KVIO *) bio->bi_private;
  kvioAddTraceRecord(kvio, THIS_LOCATION("$F($io);cb=io($io)"));
  countCompletedBios(bio);
  if ((error == 0) && isData(kvio) && isReadVIO(kvio->vio)) {
    DataKVIO *dataKVIO = kvioAsDataKVIO(kvio);
    if (!isCompressed(dataKVIO->dataVIO.mapped.state)
        && !dataKVIO->isPartial) {
      kvdoAcknowledgeDataVIO(&dataKVIO->dataVIO);
      return;
    }
  }
  kvdoContinueKvio(kvio, error);
}

/**********************************************************************/
static void startBioQueue(void *ptr)
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,38)
  BioQueueData *bioQueueData = (BioQueueData *)ptr;
  blk_start_plug(&bioQueueData->plug);
#endif
}

/**********************************************************************/
static void finishBioQueue(void *ptr)
{
  BioQueueData *bioQueueData = (BioQueueData *)ptr;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,38)
  blk_finish_plug(&bioQueueData->plug);
#else
  //on early kernels, we have to kick the queue often.
  struct request_queue *q = bdev_get_queue(bioQueueData->bdev);
  if (q->unplug_fn != NULL) {
    q->unplug_fn(q);
  }
#endif
}

static const KvdoWorkQueueType bioQueueType = {
  .start       = startBioQueue,
  .finish      = finishBioQueue,
  .actionTable = {
    { .name = "bio_compressed_data",
      .code = BIO_Q_ACTION_COMPRESSED_DATA,
      .priority = 0 },
    { .name = "bio_data",
      .code = BIO_Q_ACTION_DATA,
      .priority = 0 },
    { .name = "bio_flush",
      .code = BIO_Q_ACTION_FLUSH,
      .priority = 2 },
    { .name = "bio_high",
      .code = BIO_Q_ACTION_HIGH,
      .priority = 2 },
    { .name = "bio_metadata",
      .code = BIO_Q_ACTION_METADATA,
      .priority = 1 },
    { .name = "bio_readcache",
      .code = BIO_Q_ACTION_READCACHE,
      .priority = 0 },
    { .name = "bio_verify",
      .code = BIO_Q_ACTION_VERIFY,
      .priority = 1 },
  },
};

/**
 * Determines which bio counter to use
 *
 * @param kvio the kvio associated with the bio
 * @param bio  the bio to count
 */
static void countAllBios(KVIO *kvio, BIO *bio)
{
  KernelLayer *layer = kvio->layer;
  if (isData(kvio)) {
    countBios(&layer->biosOut, bio);
    return;
  }

  countBios(&layer->biosMeta, bio);
  if (kvio->vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
    countBios(&layer->biosJournal, bio);
  } else if (kvio->vio->type == VIO_TYPE_BLOCK_MAP) {
    countBios(&layer->biosPageCache, bio);
  }
}

/**********************************************************************/
void sendBioToDevice(KVIO *kvio, BIO *bio, TraceLocation location)
{
  /*
   * If we're skipping the read cache before doing the I/O, we're
   * doing round-robin thread selection, so even under the
   * IWS_RC_PBN_BIO_PBN strategy we can't check the correctness of the
   * bio work queue selection, only that we're running in one of them.
   */
  assertRunningInBioQueue();

  atomic64_inc(&kvio->layer->biosSubmitted);
  countAllBios(kvio, bio);
  kvioAddTraceRecord(kvio, location);
  bio->bi_next = NULL;
  generic_make_request(bio);
}

/**
 * Submits a bio to the underlying block device.  May block if the
 * device is busy.
 *
 * For metadata or if USE_BIOMAP is disabled, kvio->bioToSubmit holds
 * the BIO pointer to submit to the target device. For normal
 * data when USE_BIOMAP is enabled, kvio->biosMerged is the list of
 * all bios collected together in this group; all of them get
 * submitted. In both cases, the bi_end_io callback is invoked when
 * each I/O operation completes.
 *
 * @param item  The work item in the KVIO "owning" either the bio to
 *              submit, or the head of the bio_list to be submitted.
 **/
static void processBioMap(KvdoWorkItem *item)
{
  KVIO *kvio = workItemAsKVIO(item);
  /*
   * XXX Make these paths more regular: Should bi_bdev be set here, or
   * in the caller, or in the callback function? Should we call
   * finishBioQueue for the biomap case on old kernels?
   */
  if (USE_BIOMAP && isData(kvio)) {
    // We need to make sure to do two things here:
    // 1. Use each bio's kvio when submitting. Any other kvio is not safe
    // 2. Detach the bio list from the kvio before submitting, because it
    //    could get reused/free'd up before all bios are submitted.
    BioQueueData *bioQueueData = getWorkQueuePrivateData();
    BIO          *bio          = NULL;
    mutex_lock(&bioQueueData->lock);
    if (!bio_list_empty(&kvio->biosMerged)) {
      intMapRemove(bioQueueData->map, getBioSector(kvio->biosMerged.head));
      intMapRemove(bioQueueData->map, getBioSector(kvio->biosMerged.tail));
    }
    bio = kvio->biosMerged.head;
    bio_list_init(&kvio->biosMerged);
    mutex_unlock(&bioQueueData->lock);
    // Somewhere in the list we'll be submitting the current "kvio",
    // so drop our handle on it now.
    kvio = NULL;

    while (bio != NULL) {
      KVIO *kvioBio = bio->bi_private;
      BIO  *next    = bio->bi_next;
      bio->bi_next  = NULL;
      setBioBlockDevice(bio, kvioBio->layer->dev->bdev);
      kvioBio->bioSubmissionCallback(&kvioBio->enqueueable.workItem);
      bio = next;
    }
  } else {
    kvio->bioSubmissionCallback(&kvio->enqueueable.workItem);
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,38)
    //if journaling, kick the queue to make the requests leave faster
    BioQueueData *bioQueueData = getWorkQueuePrivateData();
    finishBioQueue(bioQueueData);
#endif
  }
}

/**
 * This function will attempt to find an already queued bio that the current
 * bio can be merged with. There are two types of merging possible, forward
 * and backward, which are distinguished by a flag that uses kernel
 * elevator terminology.
 *
 * @param map        The bio map to use for merging
 * @param kvio       The kvio we want to merge
 * @param mergeType  The type of merging we want to try
 *
 * @return the kvio to merge to, NULL if no merging is possible
 */
static KVIO *getMergeableLocked(IntMap       *map,
                                KVIO         *kvio,
                                unsigned int  mergeType)
{
  BIO         *bio         = kvio->bioToSubmit;
  sector_t     mergeSector = getBioSector(bio);
  switch (mergeType) {
  case ELEVATOR_BACK_MERGE:
    mergeSector -= VDO_SECTORS_PER_BLOCK;
    break;
  case ELEVATOR_FRONT_MERGE:
    mergeSector += VDO_SECTORS_PER_BLOCK;
    break;
  }

  KVIO *kvioMerge = intMapGet(map, mergeSector);

  if (kvioMerge != NULL) {
    if (!areWorkItemActionsEqual(&kvio->enqueueable.workItem,
                                 &kvioMerge->enqueueable.workItem)) {
      return NULL;
    } else if (bio_data_dir(bio) != bio_data_dir(kvioMerge->bioToSubmit)) {
      return NULL;
    } else if (bio_list_empty(&kvioMerge->biosMerged)) {
      return NULL;
    } else {
      switch (mergeType) {
      case ELEVATOR_BACK_MERGE:
        if (getBioSector(kvioMerge->biosMerged.tail) != mergeSector) {
          return NULL;
        }
        break;
      case ELEVATOR_FRONT_MERGE:
        if (getBioSector(kvioMerge->biosMerged.head) != mergeSector) {
          return NULL;
        }
        break;
      }
    }
  }

  return kvioMerge;
}

/**********************************************************************/
static inline unsigned int advanceBioRotor(IOSubmitter *bioData)
{
  unsigned int index = bioData->bioQueueRotor++
                       % (bioData->numBioQueuesUsed
                          * bioData->bioQueueRotationInterval);
  index /= bioData->bioQueueRotationInterval;
  return index;
}

/**********************************************************************/
static bool tryBioMapMerge(BioQueueData *bioQueueData, KVIO *kvio, BIO *bio)
{
  bool merged = false;

  mutex_lock(&bioQueueData->lock);
  KVIO *prevKvio = getMergeableLocked(bioQueueData->map, kvio,
                                      ELEVATOR_BACK_MERGE);
  KVIO *nextKvio = getMergeableLocked(bioQueueData->map, kvio,
                                      ELEVATOR_FRONT_MERGE);
  if (prevKvio == nextKvio) {
    nextKvio = NULL;
  }
  int result;
  if ((prevKvio == NULL) && (nextKvio == NULL)) {
    // no merge. just add to bioQueue
    result = intMapPut(bioQueueData->map, getBioSector(bio), kvio, true, NULL);
    // We don't care about failure of intMapPut in this case.
    result = result;
    mutex_unlock(&bioQueueData->lock);
  } else {
    if (nextKvio == NULL) {
      // Only prev. merge to  prev's tail
      intMapRemove(bioQueueData->map, getBioSector(prevKvio->biosMerged.tail));
      bio_list_merge(&prevKvio->biosMerged, &kvio->biosMerged);
      result = intMapPut(bioQueueData->map,
                         getBioSector(prevKvio->biosMerged.head),
                         prevKvio, true, NULL);
      result = intMapPut(bioQueueData->map,
                         getBioSector(prevKvio->biosMerged.tail),
                         prevKvio, true, NULL);
    } else {
      // Only next. merge to next's head
      //
      // Handle "next merge" and "gap fill" cases the same way so as to
      // reorder bios in a way that's compatible with using funnel queues
      // in work queues.  This avoids removing an existing work item.
      intMapRemove(bioQueueData->map, getBioSector(nextKvio->biosMerged.head));
      bio_list_merge_head(&nextKvio->biosMerged, &kvio->biosMerged);
      result = intMapPut(bioQueueData->map,
                         getBioSector(nextKvio->biosMerged.head),
                         nextKvio, true, NULL);
      result = intMapPut(bioQueueData->map,
                         getBioSector(nextKvio->biosMerged.tail),
                         nextKvio, true, NULL);
    }

    // We don't care about failure of intMapPut in this case.
    result = result;
    mutex_unlock(&bioQueueData->lock);
    merged = true;
  }
  return merged;
}

/**********************************************************************/
unsigned int bioQueueNumberForPBN(IOSubmitter         *ioSubmitter,
                                  PhysicalBlockNumber  pbn)
{
  unsigned int bioQueueIndex
    = ((pbn
        % (ioSubmitter->numBioQueuesUsed
           * ioSubmitter->bioQueueRotationInterval))
       / ioSubmitter->bioQueueRotationInterval);

  return bioQueueIndex;
}

/**********************************************************************/
static BioQueueData *bioQueueDataForPBN(IOSubmitter         *ioSubmitter,
                                        PhysicalBlockNumber  pbn)
{
  unsigned int bioQueueIndex = bioQueueNumberForPBN(ioSubmitter, pbn);
  return &ioSubmitter->bioQueueData[bioQueueIndex];
}

/**********************************************************************/
void enqueueBioMap(BIO                 *bio,
                   BioQAction           action,
                   KvdoWorkFunction     callback,
                   PhysicalBlockNumber  pbn)
{
  bool chooseQueueByPBN;
  switch (IO_WORK_STRATEGY) {
  case IWS_RC_PBN_BIO_PBN:
    chooseQueueByPBN = true;
    break;
  case IWS_RC_PBN_BIO_RR:
  case IWS_RC_BATCH_BIO_RR:
    chooseQueueByPBN = false;
    break;
  default:
    BUG();
  }

  KVIO *kvio                  = bio->bi_private;
  kvio->bioToSubmit           = bio;
  kvio->bioSubmissionCallback = callback;
  setupKVIOWork(kvio, processBioMap, (KvdoWorkFunction) bio->bi_end_io,
                action);

  KernelLayer  *layer = kvio->layer;
  BioQueueData *bioQueueData;

  if (chooseQueueByPBN) {
    bioQueueData = bioQueueDataForPBN(layer->ioSubmitter, pbn);
  } else {
    unsigned int bioQueueIndex = advanceBioRotor(layer->ioSubmitter);
    bioQueueData = &layer->ioSubmitter->bioQueueData[bioQueueIndex];
  }

  kvioAddTraceRecord(kvio, THIS_LOCATION("$F($io)"));

  bio->bi_next = NULL;
  bio_list_init(&kvio->biosMerged);
  bio_list_add(&kvio->biosMerged, bio);

  /*
   * Enabling of MD RAID5 mode optimizes performance for MD RAID5 storage
   * configurations.  It clears the bits for sync I/O RW flags on data block
   * bios and sets the bits for sync I/O RW flags on all journal-related
   * bios.
   *
   * This increases the frequency of full-stripe writes by altering flags of
   * submitted bios.  For workloads with write requests this increases the
   * likelihood that the MD RAID5 device will update a full stripe instead of
   * a partial stripe, thereby avoiding making read requests to the underlying
   * physical storage for purposes of parity chunk calculations.
   *
   * Setting the sync-flag on journal-related bios is expected to reduce
   * latency on journal updates submitted to an MD RAID5 device.
   */
  if (layer->mdRaid5ModeEnabled) {
    if (isData(kvio)) {
      // Clear the bits for sync I/O RW flags on data block bios.
      clearBioOperationFlagSync(bio);
    } else if ((kvio->vio->type == VIO_TYPE_RECOVERY_JOURNAL)
               || (kvio->vio->type == VIO_TYPE_SLAB_JOURNAL)) {
      // Set the bits for sync I/O RW flags on all journal-related and
      // slab-journal-related bios.
      setBioOperationFlagSync(bio);
    }
  }

  bool merged = false;
  if (USE_BIOMAP && isData(kvio)) {
    merged = tryBioMapMerge(bioQueueData, kvio, bio);
  }
  if (!merged) {
    enqueueKVIOWork(bioQueueData->queue, kvio);
  }
}

/**********************************************************************/
static void submitBioWork(KvdoWorkItem *item)
{
  assertRunningInBioQueue();

  KVIO *kvio = workItemAsKVIO(item);
  BIO  *bio  = kvio->bioToSubmit;
  sendBioToDevice(kvio, bio, THIS_LOCATION("$F($io)"));
}

/**********************************************************************/
void submitBio(BIO *bio, BioQAction action)
{
  KVIO *kvio = bio->bi_private;
  enqueueBioMap(bio, action, submitBioWork, kvio->vio->physical);
}

/**********************************************************************/
static int initializeBioQueue(BioQueueData *bioQueueData,
                              const char   *threadNamePrefix,
                              const char   *queueName,
                              unsigned int  queueNumber,
                              KernelLayer  *layer)
{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,38)
  bioQueueData->bdev        = layer->dev->bdev;
#endif
  bioQueueData->queueNumber = queueNumber;

  return makeWorkQueue(threadNamePrefix, queueName, &layer->wqDirectory,
                       layer, bioQueueData, &bioQueueType, 1,
                       &bioQueueData->queue);
}

/**********************************************************************/
int makeIOSubmitter(const char    *threadNamePrefix,
                    unsigned int   threadCount,
                    unsigned int   rotationInterval,
                    unsigned int   maxRequestsActive,
                    KernelLayer   *layer,
                    IOSubmitter  **ioSubmitterPtr)
{
  IOSubmitter *ioSubmitter;
  int result = ALLOCATE_EXTENDED(IOSubmitter,
                                 threadCount,
                                 BioQueueData,
                                 "bio submission data",
                                 &ioSubmitter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (layer->readCacheBlocks > 0) {
    unsigned int zoneCount;
    switch (IO_WORK_STRATEGY) {
    case IWS_RC_PBN_BIO_PBN:
    case IWS_RC_PBN_BIO_RR:
      zoneCount = threadCount;
      break;
    case IWS_RC_BATCH_BIO_RR:
      zoneCount = 1; // Hardcoded in readCache.c:zoneNumberForPBN!
      break;
    default:
      BUG();
    }

    result = makeReadCache(layer, layer->readCacheBlocks, zoneCount,
                           &ioSubmitter->readCache);
    if (result != VDO_SUCCESS) {
      FREE(ioSubmitter);
      return result;
    }
  }

  // Setup for each bio-submission work queue
  char queueName[MAX_QUEUE_NAME_LEN];
  ioSubmitter->bioQueueRotationInterval = rotationInterval;
  for (unsigned int i=0; i < threadCount; i++) {
    BioQueueData *bioQueueData = &ioSubmitter->bioQueueData[i];
    snprintf(queueName, sizeof(queueName), "bioQ%u", i);

    if (USE_BIOMAP) {
      mutex_init(&bioQueueData->lock);
      /*
       * One I/O operation per request, but both first & last sector numbers.
       *
       * If requests are assigned to threads round-robin, they should
       * be distributed quite evenly. But if they're assigned based on
       * PBN, things can sometimes be very uneven. So for now, we'll
       * assume that all requests *may* wind up on one thread, and
       * thus all in the same map.
       */
      result = makeIntMap(maxRequestsActive * 2, 0, &bioQueueData->map);
      if (result != 0) {
        // Clean up the partially initialized bio-queue entirely and
        // indicate that initialization failed.
        logError("bio map initialization failed %d", result);
        cleanupIOSubmitter(ioSubmitter);
        freeIOSubmitter(ioSubmitter);
        return result;
      }
    }

    result = initializeBioQueue(bioQueueData,
                                threadNamePrefix,
                                queueName,
                                i,
                                layer);
    if (result < 0) {
      // Clean up the partially initialized bio-queue entirely and
      // indicate that initialization failed.
      if (USE_BIOMAP) {
        freeIntMap(&ioSubmitter->bioQueueData[i].map);
      }
      logError("bio queue initialization failed %d", result);
      cleanupIOSubmitter(ioSubmitter);
      freeIOSubmitter(ioSubmitter);
      return result;
    }

    ioSubmitter->numBioQueuesUsed++;
  }

  *ioSubmitterPtr = ioSubmitter;

  return VDO_SUCCESS;
}

/**********************************************************************/
void cleanupIOSubmitter(IOSubmitter *ioSubmitter)
{
  for (int i=ioSubmitter->numBioQueuesUsed - 1; i >= 0; i--) {
    finishWorkQueue(ioSubmitter->bioQueueData[i].queue);
  }
}

/**********************************************************************/
void freeIOSubmitter(IOSubmitter *ioSubmitter)
{
  for (int i = ioSubmitter->numBioQueuesUsed - 1; i >= 0; i--) {
    ioSubmitter->numBioQueuesUsed--;
    freeWorkQueue(&ioSubmitter->bioQueueData[i].queue);
    if (USE_BIOMAP) {
      freeIntMap(&ioSubmitter->bioQueueData[i].map);
    }
  }
  freeReadCache(&ioSubmitter->readCache);
  FREE(ioSubmitter);
}

/**********************************************************************/
void getBioWorkQueueReadCacheStats(IOSubmitter    *ioSubmitter,
                                   ReadCacheStats *totalledStats)
{
  *totalledStats = readCacheGetStats(ioSubmitter->readCache);
}

/**********************************************************************/
void dumpBioWorkQueue(IOSubmitter *ioSubmitter)
{
  for (int i=0; i < ioSubmitter->numBioQueuesUsed; i++) {
    dumpWorkQueue(ioSubmitter->bioQueueData[i].queue);
  }
}


/**********************************************************************/
ReadCache *getIOSubmitterReadCache(IOSubmitter *ioSubmitter)
{
  return ioSubmitter->readCache;
}

/**********************************************************************/
void enqueueByPBNBioWorkItem(IOSubmitter         *ioSubmitter,
                             PhysicalBlockNumber  pbn,
                             KvdoWorkItem        *workItem)
{
  enqueueWorkQueue(bioQueueDataForPBN(ioSubmitter, pbn)->queue,
                   workItem);
}

/**********************************************************************/
void enqueueBioWorkItem(IOSubmitter *ioSubmitter, KvdoWorkItem *workItem)
{
  unsigned int bioQueueIndex = advanceBioRotor(ioSubmitter);
  enqueueWorkQueue(ioSubmitter->bioQueueData[bioQueueIndex].queue,
                   workItem);
}

/**********************************************************************/
void assertRunningInBioQueue(void)
{
  ASSERT_LOG_ONLY(!in_interrupt(), "not in interrupt context");
  ASSERT_LOG_ONLY(strnstr(current->comm, "bioQ", TASK_COMM_LEN) != NULL,
                  "running in bio submission work queue thread");
}

/**********************************************************************/
static inline IOSubmitter *bioQueueToSubmitter(BioQueueData *bioQueue)
{
  BioQueueData *firstBioQueue = bioQueue - bioQueue->queueNumber;
  IOSubmitter *submitter = container_of(firstBioQueue, IOSubmitter,
                                        bioQueueData[0]);
  return submitter;
}

/**********************************************************************/
void assertRunningInBioQueueForPBN(PhysicalBlockNumber pbn)
{
  assertRunningInBioQueue();

  BioQueueData *thisQueue = getCurrentBioQueueData();
  IOSubmitter *submitter = bioQueueToSubmitter(thisQueue);
  unsigned int computedQueueNumber = bioQueueNumberForPBN(submitter, pbn);
  ASSERT_LOG_ONLY(thisQueue->queueNumber == computedQueueNumber,
                  "running in correct bio queue (%u vs %u) for PBN %" PRIu64,
                  thisQueue->queueNumber, computedQueueNumber, pbn);
}
