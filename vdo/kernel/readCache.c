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
 */
#include "readCache.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "util/atomic.h"

#include "compressedBlock.h"
#include "intMap.h"
#include "lz4.h"
#include "statusCodes.h"
#include "waitQueue.h"

#include "bio.h"
#include "dataKVIO.h"
#include "histogram.h"
#include "ioSubmitterInternals.h"

/*
 * Zoned read-cache implementation.
 *
 * The read cache holds data matching certain physical blocks on the
 * target storage device. The address space is subdivided into zones
 * associated with different threads.
 *
 * We cannot queue up VIOs here that want cache slots that aren't
 * available yet. VIOs that come to us looking for cache slots can be
 * holding PBN locks at the time, and (in other cases) VIOs we've
 * already given cache slots to can later go attempt to acquire PBN
 * locks. We might be able to do queueing as long as each zone has at
 * least (more than?) maxRequests/2 slots, but it's probably better to
 * make each zone capable of handling all the requests at one time,
 * expending the extra megabytes (in multi-zone configurations),
 * keeping the cache code a little simpler, and avoiding the whole
 * deadlock question.
 */

static const PhysicalBlockNumber INVALID_BLOCK = (PhysicalBlockNumber) -1;

typedef enum {
  RC_FREE,                      // entry is free
  RC_RECLAIMABLE,               // entry is reclaimable
  RC_IN_USE,                    // in use but no physical block number
  RC_IN_USE_PBN,                // in use, has physical block number
  RC_MAX_STATES
} CacheEntryState;

typedef struct readCacheEntryRelease {
  KvdoWorkItem  workItem;
  Atomic32      releases;
} ReadCacheEntryRelease;

typedef struct readCacheZone ReadCacheZone;

struct readCacheEntry {
  struct list_head       list;
  int                    entryNum;
  bool                   dataValid;
  Atomic32               refCount;
  uint64_t               hits;
  PhysicalBlockNumber    pbn;
  char                  *dataBlock;
  int                    error;
  BIO                   *bio;
  ReadCacheZone         *zone;
  spinlock_t             waitLock;
  WaitQueue              callbackWaiters;
  ReadCacheEntryRelease  release;
};

enum {
  THREAD_SAFE = 0
};

struct readCacheZone {
  ReadCacheStats     stats;
  KernelLayer       *layer;
  unsigned int       numEntries;
  spinlock_t         lock;
  struct list_head   busyList;
  struct list_head   reclaimList;
  struct list_head   freeList;
  IntMap            *pbnMap;
  ReadCacheEntry   **blockMap;
  char              *dataBlocks;
};

struct readCache {
  KernelLayer   *layer;
  unsigned int   zoneCount;
  ReadCacheZone *zones[];
};

static void dumpReadCacheEntry(char tag, ReadCacheEntry *entry);

/**********************************************************************/
static inline uint32_t getCacheEntryRefCount(ReadCacheZone  *zone,
                                             ReadCacheEntry *cacheEntry)
{
  uint32_t result;

  if (THREAD_SAFE) {
    result = atomicLoad32(&cacheEntry->refCount);
  } else {
    result = relaxedLoad32(&cacheEntry->refCount);
  }

  return result;
}

/**********************************************************************/
static inline uint32_t addToCacheEntryRefCount(ReadCacheZone  *zone,
                                               ReadCacheEntry *cacheEntry,
                                               int32_t         delta)
{
  uint32_t result;

  if (THREAD_SAFE) {
    result = atomicAdd32(&cacheEntry->refCount, delta);
  } else {
    result = relaxedAdd32(&cacheEntry->refCount, delta);
  }

  return result;
}

/**********************************************************************/
static inline void setCacheEntryRefCount(ReadCacheZone  *zone,
                                         ReadCacheEntry *cacheEntry,
                                         uint32_t        newValue)
{
  if (THREAD_SAFE) {
    atomicStore32(&cacheEntry->refCount, newValue);
  } else {
    relaxedStore32(&cacheEntry->refCount, newValue);
  }
}

/**********************************************************************/
static inline void lock(ReadCacheZone *zone)
{
  if (THREAD_SAFE) {
    spin_lock(&zone->lock);
  }
}

/**********************************************************************/
static inline void unlock(ReadCacheZone *zone)
{
  if (THREAD_SAFE) {
    spin_unlock(&zone->lock);
  }
}

/**
 * Logs the contents of a ReadCacheEntry at info level, for debugging.
 *
 * @param label       Arbitrary label for the log message
 * @param cacheEntry  The cache entry to log
 */
static inline void logCacheEntry(const char *label, ReadCacheEntry *cacheEntry)
{
  if (cacheEntry == NULL) {
    logInfo("%s: cacheEntry=NULL", label);
    return;
  }

  uint32_t refCount = relaxedLoad32(&cacheEntry->refCount);
  if (cacheEntry->pbn == INVALID_BLOCK) {
    logInfo("%s: entryNum=%d refCount=%d pbn=INVALID",
            label, cacheEntry->entryNum, refCount);
  } else {
    logInfo("%s: entryNum=%d refCount=%d pbn=%" PRIu64,
            label, cacheEntry->entryNum, refCount, cacheEntry->pbn);
  }
}

/**
 * Returns the state of a ReadCacheEntry.
 *
 * Can only be called with appropriate synchronization.
 *
 * @param cacheEntry  The cache entry
 *
 * @return The entry's state
 */
static inline CacheEntryState getState(ReadCacheEntry *cacheEntry)
{
  if (relaxedLoad32(&cacheEntry->refCount) == 0) {
    return (cacheEntry->pbn == INVALID_BLOCK) ? RC_FREE : RC_RECLAIMABLE;
  } else {
    return (cacheEntry->pbn == INVALID_BLOCK) ? RC_IN_USE : RC_IN_USE_PBN;
  }
}

/**
 * Given a ReadCacheEntry, decrement its reference count, moving it
 * to the free or reclaim list if the reference count reaches 0.
 *
 * @param cacheEntry  The cache entry
 */
static void releaseBlockInternal(ReadCacheEntry *cacheEntry)
{
  ReadCacheZone *zone = cacheEntry->zone;
  if (ASSERT(getCacheEntryRefCount(zone, cacheEntry) > 0,
             "freeing in-use block")) {
    return;
  }
  if (addToCacheEntryRefCount(zone, cacheEntry, -1) == 0) {
    lock(zone);
    if (getCacheEntryRefCount(zone, cacheEntry) == 0) {
      if (cacheEntry->pbn != INVALID_BLOCK) {
        list_move_tail(&cacheEntry->list, &zone->reclaimList);
      } else {
        list_move_tail(&cacheEntry->list, &zone->freeList);
      }
    }
    unlock(zone);
  }
}

/**********************************************************************/
static void assertRunningInRCQueueForPBN(ReadCache           *cache,
                                         PhysicalBlockNumber  pbn)
{
  assertRunningInBioQueueForPBN(pbn);
}

/**
 * Return a pointer to the requested zone.
 *
 * @param readCache  The read cache
 * @param index      The zone number
 *
 * @return The zone pointer
 **/
static ReadCacheZone *getReadCacheZone(ReadCache    *readCache,
                                       unsigned int  index)
{
  BUG_ON(index >= readCache->zoneCount);
  return readCache->zones[index];
}

/**********************************************************************/
static ReadCacheZone *zoneForPBN(ReadCache *cache, PhysicalBlockNumber pbn)
{
  unsigned int zone = bioQueueNumberForPBN(cache->layer->ioSubmitter, pbn);
  return getReadCacheZone(cache, zone);
}

/**
 * Sets the physical block number of a cacheEntry and enters its
 * mapping into the PBN map dependent on the replace flag.
 *
 * @param zone        The read cache zone
 * @param cacheEntry  The cache entry
 * @param pbn         The physical block number
 */
static int setBlockPBNInternal(ReadCacheZone       *zone,
                               ReadCacheEntry      *cacheEntry,
                               PhysicalBlockNumber  pbn)
{
  assertRunningInRCQueueForPBN(zone->layer->ioSubmitter->readCache, pbn);

  lock(zone);
  int result = intMapPut(zone->pbnMap, pbn, cacheEntry, true, NULL);
  if (result != VDO_SUCCESS) {
    unlock(zone);
    return result;
  }
  cacheEntry->pbn = pbn;
  unlock(zone);
  return VDO_SUCCESS;
}

/**********************************************************************/
static ReadCacheEntry *findBlockForReadInternal(ReadCacheZone       *zone,
                                                PhysicalBlockNumber  pbn)
{
  lock(zone);
  zone->stats.accesses++;
  ReadCacheEntry *cacheEntry = intMapGet(zone->pbnMap, pbn);
  if (cacheEntry != NULL) {
    if (getState(cacheEntry) == RC_RECLAIMABLE) {
      list_move_tail(&cacheEntry->list, &zone->busyList);
    }
    zone->stats.hits++;
    if (cacheEntry->dataValid) {
      zone->stats.dataHits++;
    }
    addToCacheEntryRefCount(zone, cacheEntry, 1);
    cacheEntry->hits++;
  }
  unlock(zone);
  return cacheEntry;
}

/**********************************************************************/
static ReadCacheStats readCacheZoneGetStats(ReadCacheZone *zone)
{
  ReadCacheStats stats = {
                          .accesses = 0,
                          .hits     = 0
                         };

  if (zone != NULL) {
    lock(zone);
    stats = zone->stats;
    unlock(zone);
  }

  return stats;
}

/**********************************************************************/
ReadCacheStats readCacheGetStats(ReadCache *readCache)
{
  ReadCacheStats totalledStats;

  // Sum the read cache stats.
  totalledStats.accesses = 0;
  totalledStats.dataHits = 0;
  totalledStats.hits     = 0;
  if (readCache == NULL) {
    return totalledStats;
  }

  for (unsigned int i = 0; i < readCache->zoneCount; i++) {
    ReadCacheStats stats = readCacheZoneGetStats(readCache->zones[i]);
    totalledStats.accesses += stats.accesses;
    totalledStats.dataHits += stats.dataHits;
    totalledStats.hits     += stats.hits;
  }

  return totalledStats;
}

/**********************************************************************/
static int getScratchBlockInternal(ReadCacheZone   *zone,
                                   ReadCacheEntry **cacheEntryPtr)
{
  ReadCacheEntry *cacheEntry;
  lock(zone);
  if (list_empty(&zone->freeList)) {
    if (unlikely(list_empty(&zone->reclaimList))) {
      unlock(zone);
      ASSERT_LOG_ONLY(false,
                      "read cache has free scratch blocks");
      return VDO_READ_CACHE_BUSY;
    }
    cacheEntry = list_first_entry(&zone->reclaimList,
                                  ReadCacheEntry, list);
    intMapRemove(zone->pbnMap, cacheEntry->pbn);
    cacheEntry->pbn = INVALID_BLOCK;
    ASSERT_LOG_ONLY(relaxedLoad32(&cacheEntry->refCount) == 0,
                    "reclaim block has zero refcount");
  } else {
    cacheEntry = list_first_entry(&zone->freeList, ReadCacheEntry,
                                  list);
    ASSERT_LOG_ONLY(relaxedLoad32(&cacheEntry->refCount) == 0,
                    "free block has zero refcount");
  }
  list_move(&cacheEntry->list, &zone->busyList);
  unlock(zone);
  setCacheEntryRefCount(zone, cacheEntry, 1);
  cacheEntry->hits = 0;
  cacheEntry->dataValid = false;
  ASSERT_LOG_ONLY(cacheEntry->pbn == INVALID_BLOCK,
                  "returned block has no pbn");
  *cacheEntryPtr = cacheEntry;
  return VDO_SUCCESS;
}

/**********************************************************************/
static int allocateBlockForReadInternal(ReadCacheZone        *zone,
                                        PhysicalBlockNumber   pbn,
                                        ReadCacheEntry      **cacheEntryPtr)
{
  assertRunningInRCQueueForPBN(zone->layer->ioSubmitter->readCache, pbn);

  ReadCacheEntry *cacheEntry = findBlockForReadInternal(zone, pbn);
  if (cacheEntry == NULL) {
    int result = getScratchBlockInternal(zone, &cacheEntry);
    if (result != VDO_SUCCESS) {
      return result;
    }
    setBlockPBNInternal(zone, cacheEntry, pbn);
  }
  *cacheEntryPtr = cacheEntry;
  return VDO_SUCCESS;
}

/**
 * Uncompress the data that's just been read or fetched from the cache
 * and then call back the requesting DataKVIO.
 *
 * @param workItem  The DataKVIO requesting the data
 **/
static void uncompressReadBlock(KvdoWorkItem *workItem)
{
  DataKVIO  *dataKVIO  = workItemAsDataKVIO(workItem);
  ReadBlock *readBlock = &dataKVIO->readBlock;
  BlockSize  blockSize = VDO_BLOCK_SIZE;

  // The DataKVIO's scratch block will be used to contain the
  // uncompressed data.
  uint16_t fragmentOffset, fragmentSize;
  char *compressedData = readBlock->data;
  int result = getCompressedBlockFragment(readBlock->mappingState,
                                          compressedData, blockSize,
                                          &fragmentOffset,
                                          &fragmentSize);
  if (result != VDO_SUCCESS) {
    logDebug("%s: frag err %d", __func__, result);
    readBlock->status = result;
    readBlock->callback(dataKVIO);
    return;
  }

  char *fragment = compressedData + fragmentOffset;
  int size = LZ4_uncompress_unknownOutputSize(fragment, dataKVIO->scratchBlock,
                                              fragmentSize, blockSize);
  if (size == blockSize) {
    readBlock->data = dataKVIO->scratchBlock;
  } else {
    logDebug("%s: lz4 error", __func__);
    readBlock->status = VDO_INVALID_FRAGMENT;
  }

  readBlock->callback(dataKVIO);
}

/**
 * Now that we have gotten the data, either from the cache or storage,
 * uncompress the data if necessary and then call back the requesting DataKVIO.
 *
 * @param dataKVIO  The DataKVIO requesting the data
 * @param result    The result of the read operation
 **/
static void completeRead(DataKVIO *dataKVIO, int result)
{
  ReadBlock *readBlock = &dataKVIO->readBlock;
  readBlock->status = result;

  if ((result == VDO_SUCCESS) && isCompressed(readBlock->mappingState)) {
    launchDataKVIOOnCPUQueue(dataKVIO, uncompressReadBlock, NULL,
                             CPU_Q_ACTION_COMPRESS_BLOCK);
    return;
  }

  readBlock->callback(dataKVIO);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Callback for a bio doing a read with no cache.
 *
 * @param bio     The bio
 */
static void readBioCallback(BIO *bio)
#else
/**
 * Callback for a bio doing a read with no cache.
 *
 * @param bio     The bio
 * @param result  The result of the read operation
 */
static void readBioCallback(BIO *bio, int result)
#endif
{
  KVIO *kvio = (KVIO *) bio->bi_private;
  DataKVIO *dataKVIO = kvioAsDataKVIO(kvio);
  dataKVIO->readBlock.data = dataKVIO->readBlock.buffer;
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  countCompletedBios(bio);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  completeRead(dataKVIO, getBioResult(bio));
#else
  completeRead(dataKVIO, result);
#endif
}

/**
 * Callback for all waiters on a cache block read.
 *
 * @param waiter   The waiter
 * @param context  The context passed to callback
 */
static void cacheBlockReadWaiterCallback(Waiter *waiter, void *context)
{
  completeRead(dataVIOAsDataKVIO(waiterAsDataVIO(waiter)), *((int *) context));
}

/**********************************************************************/
static void readBlockCompletionWork(KvdoWorkItem *item)
{
  DataKVIO *dataKVIO = workItemAsDataKVIO(item);
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));

  int             error      = dataKVIO->readBlock.status;
  ReadCacheEntry *cacheEntry = dataKVIO->readBlock.cacheEntry;

  // We're going to have a single callback here, since there is now no
  // difference between success and failure.
  spin_lock(&cacheEntry->waitLock);
  cacheEntry->dataValid = (error == 0);
  notifyAllWaiters(&cacheEntry->callbackWaiters,
                   cacheBlockReadWaiterCallback, &error);
  spin_unlock(&cacheEntry->waitLock);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Callback for a bio which did a read to populate a cache entry.
 *
 * @param bio    The bio
 */
static void readCacheBioCallback(BIO *bio)
#else
/**
 * Callback for a bio which did a read to populate a cache entry.
 *
 * @param bio    The bio
 * @param error  The result of the read operation
 */
static void readCacheBioCallback(BIO *bio, int error)
#endif
{
  KVIO *kvio = (KVIO *) bio->bi_private;
  DataKVIO *dataKVIO = kvioAsDataKVIO(kvio);
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  countCompletedBios(bio);

  // Set read block operation back to nothing so bio counting works
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  dataKVIO->readBlock.status = getBioResult(bio);
#else
  dataKVIO->readBlock.status = error;
#endif
  enqueueDataKVIO(dataKVIO, readBlockCompletionWork, NULL,
                  REQ_Q_ACTION_VIO_CALLBACK);
}

/**
 * Removes any entry for the specified physical block number from
 * the read cache's known PBNs.
 *
 * @param readCache  The read cache
 * @param pbn        The physical block number
 **/
static void readCacheInvalidatePBN(ReadCache           *readCache,
                                   PhysicalBlockNumber  pbn)
{
  if (ASSERT(readCache != NULL, "specified read cache")) {
    return;
  }

  assertRunningInRCQueueForPBN(readCache, pbn);

  ReadCacheZone *zone = zoneForPBN(readCache, pbn);
  lock(zone);
  intMapRemove(zone->pbnMap, pbn);
  unlock(zone);
}

/**********************************************************************/
static void invalidatePBNAndContinueVIO(KvdoWorkItem *item)
{
  KVIO *kvio = workItemAsKVIO(item);
  // readCacheInvalidatePBN will check that we're on the correct queue.
  readCacheInvalidatePBN(kvio->layer->ioSubmitter->readCache,
                         kvio->vio->physical);
  kvdoEnqueueVIOCallback(kvio);
}

/**
 * Run a read cache-related work action in the appropriate work queue.
 * The caller surrenders ownership of the work item object.
 *
 * Callers working with KVIOs probably should use
 * runReadCacheActionOnKVIO instead.
 *
 * @param layer     The kernel layer
 * @param pbn       The physical block number
 * @param workItem  The work item to enqueue
 **/
static void runReadCacheWorkItem(KernelLayer         *layer,
                                 PhysicalBlockNumber  pbn,
                                 KvdoWorkItem        *workItem)
{
  // The work item is likely *not* a KVIO, so no I/O will happen, so
  // don't get involved with the bio map code.
  enqueueByPBNBioWorkItem(layer->ioSubmitter, pbn, workItem);
}

/**********************************************************************/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
static void invalidatePBNBioCallback(BIO *bio)
#else
static void invalidatePBNBioCallback(BIO *bio, int error)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  int error = getBioResult(bio);
#endif

  bio->bi_end_io = completeAsyncBio;

  KVIO *kvio = (KVIO *) bio->bi_private;
  kvioAddTraceRecord(kvio, THIS_LOCATION("$F($io);cb=io($io)"));
  countCompletedBios(bio);
  if (unlikely(error)) {
    setCompletionResult(vioAsCompletion(kvio->vio), error);
  }

  setupKVIOWork(kvio, invalidatePBNAndContinueVIO, NULL,
                BIO_Q_ACTION_READCACHE);
  runReadCacheWorkItem(kvio->layer, kvio->vio->physical,
                       &kvio->enqueueable.workItem);
}

/**********************************************************************/
void invalidateCacheAndSubmitBio(KVIO *kvio, BioQAction action)
{
  BIO *bio = kvio->bio;
  BUG_ON(bio->bi_end_io != completeAsyncBio);
  if (kvio->layer->ioSubmitter->readCache != NULL) {
    bio->bi_end_io = invalidatePBNBioCallback;
  }
  BUG_ON(bio->bi_private != kvio);
  submitBio(bio, action);
}

/**
 * Free a ReadCacheZone.
 *
 * @param readCacheZonePtr  The ReadCacheZone to free
 **/
static void freeReadCacheZone(ReadCacheZone **readCacheZonePtr)
{
  if (*readCacheZonePtr == NULL) {
    return;
  }

  ReadCacheZone *zone = *readCacheZonePtr;

  // At shutdown, all entries should have refcount 0, and none should
  // be "busy".
  ReadCacheEntry *cacheEntry;
  ReadCacheEntry *temp;
  list_for_each_entry_safe(cacheEntry, temp, &zone->freeList, list) {
    unsigned int refCount = getCacheEntryRefCount(zone, cacheEntry);
    ASSERT_LOG_ONLY(refCount == 0,
                    "refcount (%u) of 'free' cache entry %p is 0",
                    refCount, cacheEntry);
    freeBio(cacheEntry->bio, zone->layer);
    FREE(cacheEntry);
  }
  bool first = true; // avoid redundant verbosity for same error
  list_for_each_entry_safe(cacheEntry, temp, &zone->reclaimList, list) {
    unsigned int refCount = getCacheEntryRefCount(zone, cacheEntry);
    if (refCount != 0) {
      if (first) {
        ASSERT_LOG_ONLY(refCount == 0,
                        "refcount (%u) of 'reclaimable' cache entry %p is 0",
                        refCount, cacheEntry);
        first = false;
      }
      // Just one line per entry
      dumpReadCacheEntry('R', cacheEntry);
    }
    freeBio(cacheEntry->bio, zone->layer);
    FREE(cacheEntry);
  }
  ASSERT_LOG_ONLY(list_empty(&zone->busyList),
                  "'busy' cache entry list is empty at shutdown");
  list_for_each_entry_safe(cacheEntry, temp, &zone->busyList, list) {
    dumpReadCacheEntry('B', cacheEntry);
    freeBio(cacheEntry->bio, zone->layer);
    FREE(cacheEntry);
  }
  freeIntMap(&zone->pbnMap);
  FREE(zone->dataBlocks);
  FREE(zone->blockMap);
  FREE(zone);

  *readCacheZonePtr = NULL;
}

/**********************************************************************/
void freeReadCache(ReadCache **readCachePtr)
{
  ReadCache *readCache = *readCachePtr;
  if (readCache == NULL) {
    return;
  }
  *readCachePtr = NULL;

  for (unsigned int i = 0; i < readCache->zoneCount; i++) {
    freeReadCacheZone(&readCache->zones[i]);
  }
  FREE(readCache);
}

/**
 * Allocate and initialize a ReadCacheZone.
 *
 * @param [in]  layer             The associated kernel layer
 * @param [in]  zoneNumber        The zone number
 * @param [in]  numEntries        The size of the read cache
 * @param [out] readCacheZonePtr  The new ReadCacheZone
 *
 * @return success or an error code
 **/
static int makeReadCacheZone(KernelLayer    *layer,
                             unsigned int    zoneNumber,
                             unsigned int    numEntries,
                             ReadCacheZone **readCacheZonePtr)
{
  ReadCacheZone *zone;
  int result = ALLOCATE(1, ReadCacheZone, "read cache zone", &zone);
  if (result != VDO_SUCCESS) {
    return result;
  }
  zone->numEntries = numEntries;
  result = ALLOCATE(((size_t)zone->numEntries * (size_t)VDO_BLOCK_SIZE),
                    char, "read cache data", &zone->dataBlocks);
  if (result != VDO_SUCCESS) {
    FREE(zone);
    return result;
  }
  result = ALLOCATE(zone->numEntries, ReadCacheEntry *,
                    "read cache block map", &zone->blockMap);
  if (result != VDO_SUCCESS) {
    FREE(zone->dataBlocks);
    FREE(zone);
    return result;
  }
  spin_lock_init(&zone->lock);
  INIT_LIST_HEAD(&zone->busyList);
  INIT_LIST_HEAD(&zone->reclaimList);
  INIT_LIST_HEAD(&zone->freeList);
  result = makeIntMap(zone->numEntries, 0, &zone->pbnMap);
  if (result != VDO_SUCCESS) {
    FREE(zone->dataBlocks);
    FREE(zone);
    return result;
  }

  zone->layer = layer;
  for (int i = 0; i < zone->numEntries; i++) {
    ReadCacheEntry *cacheEntry;
    result = ALLOCATE(1, ReadCacheEntry, "read cache entry", &cacheEntry);
    if (result != VDO_SUCCESS) {
      freeReadCacheZone(&zone);
      return result;
    }
    cacheEntry->pbn = INVALID_BLOCK;
    cacheEntry->dataBlock = zone->dataBlocks + ((uint64_t)i * VDO_BLOCK_SIZE);
    result = createBio(zone->layer, cacheEntry->dataBlock, &cacheEntry->bio);
    if (result != VDO_SUCCESS) {
      FREE(cacheEntry);
      freeReadCacheZone(&zone);
      return result;
    }
    spin_lock_init(&cacheEntry->waitLock);
    cacheEntry->zone = zone;
    cacheEntry->entryNum = i;
    zone->blockMap[i] = cacheEntry;
    list_add(&cacheEntry->list, &zone->freeList);
  }

  *readCacheZonePtr = zone;

  return VDO_SUCCESS;
}

/**********************************************************************/
int makeReadCache(KernelLayer   *layer,
                  unsigned int   numEntries,
                  unsigned int   zoneCount,
                  ReadCache    **readCachePtr)
{
  int result;

  ReadCache *readCache;
  result = ALLOCATE_EXTENDED(ReadCache, zoneCount, ReadCacheZone *,
                             "read cache", &readCache);
  if (result != VDO_SUCCESS) {
    return result;
  }
  readCache->layer     = layer;
  readCache->zoneCount = zoneCount;
  for (unsigned int i = 0; i < zoneCount; i++) {
    result = makeReadCacheZone(layer, i, numEntries, &readCache->zones[i]);
    if (result != VDO_SUCCESS) {
      freeReadCache(&readCache);
      return result;
    }
  }
  *readCachePtr = readCache;
  return VDO_SUCCESS;
}

/**********************************************************************/
static void readCacheZoneReleaseBlockWork(KvdoWorkItem *item)
{
  ReadCacheEntryRelease *release
    = container_of(item, ReadCacheEntryRelease, workItem);
  ReadCacheEntry *cacheEntry = container_of(release, ReadCacheEntry, release);

  while (atomicAdd32(&cacheEntry->release.releases, -1) > 0) {
    releaseBlockInternal(cacheEntry);
  }
  releaseBlockInternal(cacheEntry);
}

/**
 * Returns a block addressed by the given physical block number from
 * the cache if possible. If the block is not found in the cache,
 * issues a read to the underlying device into a free block in the
 * cache. Finally calls the callback provided in the DataKVIO's ReadBlock's
 * callback.
 *
 * The physical block number and priority must already be set in the
 * ReadBlock's fields as well. (The only caller of this function
 * is kvdoReadBlock() which will have set those fields.)
 *
 * If an error occurs along the way, ReadBlock.status is set and
 * some operations may be skipped, but the callback is still invoked.
 *
 * @param item  The DataKVIO
 **/
static void readCacheBlockCallback(KvdoWorkItem *item)
{
  DataKVIO      *dataKVIO  = workItemAsDataKVIO(item);
  ReadBlock     *readBlock = &dataKVIO->readBlock;
  KernelLayer   *layer     = getLayerFromDataKVIO(dataKVIO);
  ReadCache     *readCache = layer->ioSubmitter->readCache;
  ReadCacheZone *zone      = zoneForPBN(readCache, readBlock->pbn);

  if (ASSERT(zone != NULL, "specified read cache")) {
    completeRead(dataKVIO, VDO_BAD_CONFIGURATION);
    return;
  }

  assertRunningInRCQueueForPBN(readCache, readBlock->pbn);

  ReadCacheEntry *cacheEntry = NULL;
  int result = allocateBlockForReadInternal(zone, readBlock->pbn, &cacheEntry);
  if (result != VDO_SUCCESS) {
    completeRead(dataKVIO, result);
    return;
  }

  readBlock->cacheEntry = cacheEntry;
  readBlock->data       = cacheEntry->dataBlock;

  spin_lock(&cacheEntry->waitLock);
  if (cacheEntry->dataValid) {
    spin_unlock(&cacheEntry->waitLock);
    completeRead(dataKVIO, VDO_SUCCESS);
    return;
  }

  bool issueIO = !hasWaiters(&cacheEntry->callbackWaiters);
  result = enqueueDataVIO(&cacheEntry->callbackWaiters, &dataKVIO->dataVIO,
                          THIS_LOCATION("$F($io)"));
  spin_unlock(&cacheEntry->waitLock);

  if (result != VDO_SUCCESS) {
    completeRead(dataKVIO, result);
    return;
  }

  if (!issueIO) {
    return;
  }

  BIO *bio = cacheEntry->bio;
  resetBio(bio, layer);
  setBioSector(bio, blockToSector(layer, readBlock->pbn));
  bio->bi_end_io  = readCacheBioCallback;
  bio->bi_private = &dataKVIO->kvio;

  logDebug("%s: submitting read request for pbn %" PRIu64, __func__,
           readBlock->pbn);

  sendBioToDevice(dataKVIOAsKVIO(dataKVIO), bio, THIS_LOCATION("$F($io)"));
}

/**********************************************************************/
void kvdoReadBlock(DataVIO             *dataVIO,
                   PhysicalBlockNumber  location,
                   BlockMappingState    mappingState,
                   ReadBlockOperation   operation,
                   DataKVIOCallback     callback)
{
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));

  BioQAction action = BIO_Q_ACTION_DATA;
  switch (operation) {
  case READ_NO_OPERATION:
    logError("unexpected ReadBlockOperation: %d", operation);
    break;

  case READ_COMPRESSED_DATA:
    action = BIO_Q_ACTION_COMPRESSED_DATA;
    break;

  case READ_VERIFY_DEDUPE:
    action = BIO_Q_ACTION_VERIFY;
    break;

  default:
    logError("undefined ReadBlockOperation: %d", operation);
    break;
  }

  DataKVIO    *dataKVIO  = dataVIOAsDataKVIO(dataVIO);
  ReadBlock   *readBlock = &dataKVIO->readBlock;
  KernelLayer *layer     = getLayerFromDataKVIO(dataKVIO);
  runReadCacheReleaseBlock(layer, readBlock);

  readBlock->pbn          = location;
  readBlock->callback     = callback;
  readBlock->status       = VDO_SUCCESS;
  readBlock->mappingState = mappingState;
  readBlock->action       = action;

  BUG_ON(getBIOFromDataKVIO(dataKVIO)->bi_private != &dataKVIO->kvio);
  if (readBlock->bio != NULL) {
    // Read the data directly from the device using the read bio.
    BIO *bio = readBlock->bio;
    resetBio(bio, layer);
    setBioSector(bio, blockToSector(layer, location));
    setBioOperationRead(bio);
    bio->bi_end_io = readBioCallback;
    submitBio(bio, action);
    return;
  }

  // Feed operations through the bio map to encourage sequential
  // order in case we need to actually fetch the data.
  enqueueBioMap(getBIOFromDataKVIO(dataKVIO), action, readCacheBlockCallback,
                location);
}

/**********************************************************************/
void runReadCacheReleaseBlock(KernelLayer *layer, ReadBlock *readBlock)
{
  readBlock->data = NULL;
  if (readBlock->cacheEntry == NULL) {
    return;
  }

  ReadCacheZone *zone = zoneForPBN(layer->ioSubmitter->readCache,
                                   readBlock->pbn);
  if (ASSERT(zone != NULL, "pbn maps to read cache")) {
    return;
  }

  if (atomicAdd32(&readBlock->cacheEntry->release.releases, 1) == 1) {
    KvdoWorkItem *workItem = &readBlock->cacheEntry->release.workItem;
    setupWorkItem(workItem, readCacheZoneReleaseBlockWork, NULL,
                  BIO_Q_ACTION_HIGH);
    runReadCacheWorkItem(layer, readBlock->pbn, workItem);
  }

  readBlock->cacheEntry = NULL;
}

/**********************************************************************/
static void dumpReadCacheEntry(char tag, ReadCacheEntry *entry)
{
  /*
   * We may be logging a couple thousand of these lines, and in some
   * circumstances syslogd may have trouble keeping up, so keep it
   * BRIEF rather than user-friendly.
   */
  spin_lock(&entry->waitLock);

  Waiter *first = getFirstWaiter(&entry->callbackWaiters);
  char *maybeWaiters = (first != NULL) ? " waiters" : "";

  /*
   * Message format:
   *
   * #num B(usy)/R(ecl)/F(ree)[I(nvalid)] Refcount PBN @addr [err##]
   *
   * error==0 is the common case by far, so it's worth omitting it.
   */
  uint32_t refCount = relaxedLoad32(&entry->refCount);
  if (entry->error == 0) {
    logInfo(" #%d %c%s R%u P%" PRIu64 " @%p%s",
            entry->entryNum, tag,
            entry->dataValid ? "" : "I",
            refCount, entry->pbn,
            entry->dataBlock,
            maybeWaiters);
  } else {
    logInfo(" #%d %c%s R%u P%" PRIu64 " @%p err%d %s",
            entry->entryNum, tag,
            entry->dataValid ? "" : "I",
            refCount, entry->pbn,
            entry->dataBlock, entry->error,
            maybeWaiters);
  }

  if (first != NULL) {
    Waiter *waiter = first;
    do {
      DataVIO *dataVIO = waiterAsDataVIO(waiter);
      /*
       * If we knew whether we were dumping all the VIOs too, maybe we
       * could skip logging details of each waiter here.
       */
      logInfo("   DataVIO %p P%" PRIu64 " L%" PRIu64 " D%" PRIu64 " op %s",
              dataVIO, dataVIO->mapped.pbn, dataVIO->logical.lbn,
              dataVIO->duplicate.pbn, getOperationName(dataVIO));
      waiter = waiter->nextWaiter;
    } while (waiter != first);
  }

  spin_unlock(&entry->waitLock);
}

/**********************************************************************/
static void readCacheZoneDump(ReadCacheZone *zone,
                              bool           dumpBusyElements,
                              bool           dumpAllElements)
{
  if (ASSERT(zone != NULL, "specified read cache")) {
    return;
  }

  // The read cache dump is an approximation as it doesn't take the read
  // cache lock during processing.

  logInfo("Read cache %p:"
          " %" PRIu64 " accesses %" PRIu64 " hits %" PRIu64 " data hits"
          " %u entries",
          zone,
          zone->stats.accesses, zone->stats.hits, zone->stats.dataHits,
          zone->numEntries);

  unsigned int    numFreeItems    = 0;
  unsigned int    numReclaimItems = 0;
  unsigned int    numBusyItems    = 0;

  for (int i = 0; i < zone->numEntries; i++) {
    CacheEntryState state = getState(zone->blockMap[i]);
    switch (state) {
    case RC_FREE:
      numFreeItems++;
      if (dumpAllElements) {
        dumpReadCacheEntry('F', zone->blockMap[i]);
      }
      break;

    case RC_IN_USE:
    case RC_IN_USE_PBN:
      numBusyItems++;
      if (dumpBusyElements || dumpAllElements) {
        dumpReadCacheEntry('B', zone->blockMap[i]);
      }
      break;

    case RC_RECLAIMABLE:
      numReclaimItems++;
      if (dumpAllElements) {
        dumpReadCacheEntry('R', zone->blockMap[i]);
      }
      break;

    default:
      ASSERT_LOG_ONLY(false,
                      "cache entry state (%d) among expected values", state);
      break;
    }
  }

  logInfo("Read cache %p: %u free %u reclaimable %u busy",
          zone, numFreeItems, numReclaimItems, numBusyItems);
}

/**********************************************************************/
void readCacheDump(ReadCache *readCache,
                   bool       dumpBusyElements,
                   bool       dumpAllElements)
{
  if (readCache == NULL) {
    return;
  }

  for (int i = 0; i < readCache->zoneCount; i++) {
    logInfo("Read cache zone %d:", i);
    readCacheZoneDump(readCache->zones[i],
                      dumpBusyElements, dumpAllElements);
  }
}
