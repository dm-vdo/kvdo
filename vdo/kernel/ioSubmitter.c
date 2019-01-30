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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/ioSubmitter.c#11 $
 */

#include "ioSubmitter.h"

#include <linux/version.h>

#include "memoryAlloc.h"

#include "bio.h"
#include "dataKVIO.h"
#include "kernelLayer.h"
#include "logger.h"

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

/*
 * Submission of bio operations to the underlying storage device will
 * go through a separate work queue thread (or more than one) to
 * prevent blocking in other threads if the storage device has a full
 * queue. The plug structure allows that thread to do better batching
 * of requests to make the I/O more efficient.
 *
 * When multiple worker threads are used, a thread is chosen for a
 * I/O operation submission based on the PBN, so a given PBN will
 * consistently wind up on the same thread. Flush operations are
 * assigned round-robin.
 *
 * The map (protected by the mutex) collects pending I/O operations so
 * that the worker thread can reorder them to try to encourage I/O
 * request merging in the request queue underneath.
 */
struct bio_queue_data {
  KvdoWorkQueue         *queue;
  struct blk_plug        plug;
  IntMap                *map;
  struct mutex           lock;
  unsigned int           queueNumber;
};

struct ioSubmitter {
  unsigned int          numBioQueuesUsed;
  unsigned int          bioQueueRotationInterval;
  unsigned int          bioQueueRotor;
  struct bio_queue_data bioQueueData[];
};

/**********************************************************************/
static void startBioQueue(void *ptr)
{
  struct bio_queue_data *bioQueueData = (struct bio_queue_data *)ptr;
  blk_start_plug(&bioQueueData->plug);
}

/**********************************************************************/
static void finishBioQueue(void *ptr)
{
  struct bio_queue_data *bioQueueData = (struct bio_queue_data *)ptr;
  blk_finish_plug(&bioQueueData->plug);
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
 * Check that we're running normally (i.e., not in an
 * interrupt-servicing context) in an IOSubmitter bio thread.
 **/
static void assertRunningInBioQueue(void)
{
  ASSERT_LOG_ONLY(!in_interrupt(), "not in interrupt context");
  ASSERT_LOG_ONLY(strnstr(current->comm, "bioQ", TASK_COMM_LEN) != NULL,
                  "running in bio submission work queue thread");
}

/**
 * Returns the bio_queue_data pointer associated with the current thread.
 * Results are undefined if called from any other thread.
 *
 * @return the bio_queue_data pointer
 **/
static inline struct bio_queue_data *getCurrentBioQueueData(void)
{
  struct bio_queue_data *bioQueueData
    = (struct bio_queue_data *) get_work_queue_private_data();
  // Does it look like a bio queue thread?
  BUG_ON(bioQueueData == NULL);
  BUG_ON(bioQueueData->queue != get_current_work_queue());
  return bioQueueData;
}

/**********************************************************************/
static inline IOSubmitter *bioQueueToSubmitter(struct bio_queue_data *bioQueue)
{
  struct bio_queue_data *firstBioQueue = bioQueue - bioQueue->queueNumber;
  IOSubmitter *submitter = container_of(firstBioQueue, IOSubmitter,
                                        bioQueueData[0]);
  return submitter;
}

/**
 * Return the bio thread number handling the specified physical block
 * number.
 *
 * @param ioSubmitter       The I/O submitter data
 * @param pbn               The physical block number
 *
 * @return read cache zone number
 **/
static unsigned int bioQueueNumberForPBN(IOSubmitter         *ioSubmitter,
                                       PhysicalBlockNumber  pbn)
{
  unsigned int bioQueueIndex
    = ((pbn
        % (ioSubmitter->numBioQueuesUsed
           * ioSubmitter->bioQueueRotationInterval))
       / ioSubmitter->bioQueueRotationInterval);

  return bioQueueIndex;
}

/**
 * Check that we're running normally (i.e., not in an
 * interrupt-servicing context) in an IOSubmitter bio thread. Also
 * require that the thread we're running on is the correct one for the
 * supplied physical block number.
 *
 * @param pbn  The PBN that should have been used in thread selection
 **/
static void assertRunningInBioQueueForPBN(PhysicalBlockNumber pbn)
{
  assertRunningInBioQueue();

  struct bio_queue_data *thisQueue = getCurrentBioQueueData();
  IOSubmitter *submitter = bioQueueToSubmitter(thisQueue);
  unsigned int computedQueueNumber = bioQueueNumberForPBN(submitter, pbn);
  ASSERT_LOG_ONLY(thisQueue->queueNumber == computedQueueNumber,
                  "running in correct bio queue (%u vs %u) for PBN %llu",
                  thisQueue->queueNumber, computedQueueNumber, pbn);
}

/**
 * Increments appropriate counters for bio completions
 *
 * @param kvio the kvio associated with the bio
 * @param bio  the bio to count
 */
static void countAllBiosCompleted(KVIO *kvio, struct bio *bio)
{
  KernelLayer *layer = kvio->layer;
  if (isData(kvio)) {
    count_bios(&layer->biosOutCompleted, bio);
    return;
  }

  count_bios(&layer->biosMetaCompleted, bio);
  if (kvio->vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
    count_bios(&layer->biosJournalCompleted, bio);
  } else if (kvio->vio->type == VIO_TYPE_BLOCK_MAP) {
    count_bios(&layer->biosPageCacheCompleted, bio);
  }
}

/**********************************************************************/
void countCompletedBios(struct bio *bio)
{
  KVIO        *kvio  = (KVIO *)bio->bi_private;
  KernelLayer *layer = kvio->layer;
  atomic64_inc(&layer->biosCompleted);
  countAllBiosCompleted(kvio, bio);
}

/**********************************************************************/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
void completeAsyncBio(struct bio *bio)
#else
void completeAsyncBio(struct bio *bio, int error)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  int error = get_bio_result(bio);
#endif
  KVIO *kvio = (KVIO *) bio->bi_private;
  kvioAddTraceRecord(kvio, THIS_LOCATION("$F($io);cb=io($io)"));
  countCompletedBios(bio);
  if ((error == 0) && isData(kvio) && isReadVIO(kvio->vio)) {
    DataKVIO *dataKVIO = kvioAsDataKVIO(kvio);
    if (!isCompressed(dataKVIO->dataVIO.mapped.state)
        && !dataKVIO->isPartial) {
      acknowledgeDataVIO(&dataKVIO->dataVIO);
      return;
    }
  }
  kvdoContinueKvio(kvio, error);
}

/**
 * Determines which bio counter to use
 *
 * @param kvio the kvio associated with the bio
 * @param bio  the bio to count
 */
static void countAllBios(KVIO *kvio, struct bio *bio)
{
  KernelLayer *layer = kvio->layer;
  if (isData(kvio)) {
    count_bios(&layer->biosOut, bio);
    return;
  }

  count_bios(&layer->biosMeta, bio);
  if (kvio->vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
    count_bios(&layer->biosJournal, bio);
  } else if (kvio->vio->type == VIO_TYPE_BLOCK_MAP) {
    count_bios(&layer->biosPageCache, bio);
  }
}

/**
 * Update stats and tracing info, then submit the supplied bio to the
 * OS for processing.
 *
 * @param kvio      The KVIO associated with the bio
 * @param bio       The bio to submit to the OS
 * @param location  Call site location for tracing
 **/
static void sendBioToDevice(KVIO          *kvio,
                            struct bio    *bio,
                            TraceLocation  location)
{
  assertRunningInBioQueueForPBN(kvio->vio->physical);

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
 * the struct bio pointer to submit to the target device. For normal
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
  assertRunningInBioQueue();
  KVIO           *kvio = workItemAsKVIO(item);
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
    struct bio_queue_data *bioQueueData = get_work_queue_private_data();
    struct bio   *bio                   = NULL;
    mutex_lock(&bioQueueData->lock);
    if (!bio_list_empty(&kvio->biosMerged)) {
      intMapRemove(bioQueueData->map, get_bio_sector(kvio->biosMerged.head));
      intMapRemove(bioQueueData->map, get_bio_sector(kvio->biosMerged.tail));
    }
    bio = kvio->biosMerged.head;
    bio_list_init(&kvio->biosMerged);
    mutex_unlock(&bioQueueData->lock);
    // Somewhere in the list we'll be submitting the current "kvio",
    // so drop our handle on it now.
    kvio = NULL;

    while (bio != NULL) {
      KVIO *kvioBio = bio->bi_private;
      struct bio  *next    = bio->bi_next;
      bio->bi_next  = NULL;
      set_bio_block_device(bio, getKernelLayerBdev(kvioBio->layer));
      sendBioToDevice(kvioBio, bio, THIS_LOCATION("$F($io)"));
      bio = next;
    }
  } else {
    sendBioToDevice(kvio, kvio->bioToSubmit, THIS_LOCATION("$F($io)"));
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
  struct bio *bio          = kvio->bioToSubmit;
  sector_t    mergeSector  = get_bio_sector(bio);
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
        if (get_bio_sector(kvioMerge->biosMerged.tail) != mergeSector) {
          return NULL;
        }
        break;
      case ELEVATOR_FRONT_MERGE:
        if (get_bio_sector(kvioMerge->biosMerged.head) != mergeSector) {
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
static bool tryBioMapMerge(struct bio_queue_data *bioQueueData,
                           KVIO                  *kvio,
                           struct bio            *bio)
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
    result = intMapPut(bioQueueData->map, get_bio_sector(bio), kvio, true,
                       NULL);
    // We don't care about failure of intMapPut in this case.
    result = result;
    mutex_unlock(&bioQueueData->lock);
  } else {
    if (nextKvio == NULL) {
      // Only prev. merge to  prev's tail
      intMapRemove(bioQueueData->map, get_bio_sector(prevKvio->biosMerged.tail));
      bio_list_merge(&prevKvio->biosMerged, &kvio->biosMerged);
      result = intMapPut(bioQueueData->map,
                         get_bio_sector(prevKvio->biosMerged.head),
                         prevKvio, true, NULL);
      result = intMapPut(bioQueueData->map,
                         get_bio_sector(prevKvio->biosMerged.tail),
                         prevKvio, true, NULL);
    } else {
      // Only next. merge to next's head
      //
      // Handle "next merge" and "gap fill" cases the same way so as to
      // reorder bios in a way that's compatible with using funnel queues
      // in work queues.  This avoids removing an existing work item.
      intMapRemove(bioQueueData->map,
                   get_bio_sector(nextKvio->biosMerged.head));
      bio_list_merge_head(&nextKvio->biosMerged, &kvio->biosMerged);
      result = intMapPut(bioQueueData->map,
                         get_bio_sector(nextKvio->biosMerged.head),
                         nextKvio, true, NULL);
      result = intMapPut(bioQueueData->map,
                         get_bio_sector(nextKvio->biosMerged.tail),
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
static struct bio_queue_data *bioQueueDataForPBN(IOSubmitter *ioSubmitter,
                                        PhysicalBlockNumber   pbn)
{
  unsigned int bioQueueIndex = bioQueueNumberForPBN(ioSubmitter, pbn);
  return &ioSubmitter->bioQueueData[bioQueueIndex];
}

/**********************************************************************/
void submitBio(struct bio *bio, BioQAction action)
{
  KVIO *kvio                  = bio->bi_private;
  kvio->bioToSubmit           = bio;
  setupKVIOWork(kvio, processBioMap, (KvdoWorkFunction) bio->bi_end_io,
                action);

  KernelLayer  *layer = kvio->layer;
  struct bio_queue_data *bioQueueData
    = bioQueueDataForPBN(layer->ioSubmitter, kvio->vio->physical);

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
  if (layer->deviceConfig->mdRaid5ModeEnabled) {
    if (isData(kvio)) {
      // Clear the bits for sync I/O RW flags on data block bios.
      clear_bio_operation_flag_sync(bio);
    } else if ((kvio->vio->type == VIO_TYPE_RECOVERY_JOURNAL)
               || (kvio->vio->type == VIO_TYPE_SLAB_JOURNAL)) {
      // Set the bits for sync I/O RW flags on all journal-related and
      // slab-journal-related bios.
      set_bio_operation_flag_sync(bio);
    }
  }

 /*
  * Try to use the bio map to submit this bio earlier if we're already sending
  * IO for an adjacent block. If we can't use an existing pending bio, enqueue
  * an operation to run in a bio submission thread appropriate to the
  * indicated physical block number.
  */

  bool merged = false;
  if (USE_BIOMAP && isData(kvio)) {
    merged = tryBioMapMerge(bioQueueData, kvio, bio);
  }
  if (!merged) {
    enqueueKVIOWork(bioQueueData->queue, kvio);
  }
}

/**********************************************************************/
static int initializeBioQueue(struct bio_queue_data *bioQueueData,
                              const char            *threadNamePrefix,
                              const char            *queueName,
                              unsigned int           queueNumber,
                              KernelLayer           *layer)
{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,38)
  bioQueueData->bdev        = layer->dev->bdev;
#endif
  bioQueueData->queueNumber = queueNumber;

  return make_work_queue(threadNamePrefix, queueName, &layer->wqDirectory,
                         layer, bioQueueData, &bioQueueType, 1, NULL,
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
                                 struct bio_queue_data,
                                 "bio submission data",
                                 &ioSubmitter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Setup for each bio-submission work queue
  char queueName[MAX_QUEUE_NAME_LEN];
  ioSubmitter->bioQueueRotationInterval = rotationInterval;
  for (unsigned int i=0; i < threadCount; i++) {
    struct bio_queue_data *bioQueueData = &ioSubmitter->bioQueueData[i];
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
    if (result != VDO_SUCCESS) {
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
    finish_work_queue(ioSubmitter->bioQueueData[i].queue);
  }
}

/**********************************************************************/
void freeIOSubmitter(IOSubmitter *ioSubmitter)
{
  for (int i = ioSubmitter->numBioQueuesUsed - 1; i >= 0; i--) {
    ioSubmitter->numBioQueuesUsed--;
    free_work_queue(&ioSubmitter->bioQueueData[i].queue);
    if (USE_BIOMAP) {
      freeIntMap(&ioSubmitter->bioQueueData[i].map);
    }
  }
  FREE(ioSubmitter);
}

/**********************************************************************/
void dumpBioWorkQueue(IOSubmitter *ioSubmitter)
{
  for (int i=0; i < ioSubmitter->numBioQueuesUsed; i++) {
    dump_work_queue(ioSubmitter->bioQueueData[i].queue);
  }
}


/**********************************************************************/
void enqueueBioWorkItem(IOSubmitter *ioSubmitter, KvdoWorkItem *workItem)
{
  unsigned int bioQueueIndex = advanceBioRotor(ioSubmitter);
  enqueue_work_queue(ioSubmitter->bioQueueData[bioQueueIndex].queue,
                     workItem);
}
