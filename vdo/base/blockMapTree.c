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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapTree.c#33 $
 */

#include "blockMapTree.h"

#include "logger.h"

#include "blockMap.h"
#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "blockMapTreeInternals.h"
#include "constants.h"
#include "dataVIO.h"
#include "dirtyLists.h"
#include "forest.h"
#include "numUtils.h"
#include "recoveryJournal.h"
#include "referenceOperation.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "types.h"
#include "vdoInternal.h"
#include "vdoPageCache.h"
#include "vioPool.h"

enum {
  BLOCK_MAP_VIO_POOL_SIZE = 64,
};

struct page_descriptor {
  RootCount  rootIndex;
  Height     height;
  PageNumber pageIndex;
  SlotNumber slot;
} __attribute__((packed));

typedef union {
  struct page_descriptor descriptor;
  uint64_t               key;
} PageKey;

struct write_if_not_dirtied_context {
  struct block_map_tree_zone *zone;
  uint8_t                     generation;
};

/**
 * An invalid PBN used to indicate that the page holding the location of a
 * tree root has been "loaded".
 **/
const PhysicalBlockNumber INVALID_PBN = 0xFFFFFFFFFFFFFFFF;

/**
 * Convert a RingNode to a tree_page.
 *
 * @param ringNode The RingNode to convert
 *
 * @return The tree_page which owns the RingNode
 **/
static inline struct tree_page *treePageFromRingNode(RingNode *ringNode)
{
  return (struct tree_page *) ((byte *) ringNode
                               - offsetof(struct tree_page, node));
}

/**********************************************************************/
static void writeDirtyPagesCallback(RingNode *expired, void *context);

/**
 * Make vios for reading, writing, and allocating the arboreal block map.
 *
 * Implements VIOConstructor.
 **/
__attribute__((warn_unused_result))
static int makeBlockMapVIOs(PhysicalLayer  *layer,
                            void           *parent,
                            void           *buffer,
                            struct vio    **vioPtr)
{
  return createVIO(layer, VIO_TYPE_BLOCK_MAP_INTERIOR, VIO_PRIORITY_METADATA,
                   parent, buffer, vioPtr);
}

/**********************************************************************/
int initializeTreeZone(struct block_map_zone *zone,
                       PhysicalLayer         *layer,
                       BlockCount             eraLength)
{
  STATIC_ASSERT_SIZEOF(struct page_descriptor, sizeof(uint64_t));
  struct block_map_tree_zone *treeZone = &zone->treeZone;
  treeZone->mapZone                    = zone;

  int result = make_dirty_lists(eraLength, writeDirtyPagesCallback, treeZone,
                                &treeZone->dirtyLists);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = make_int_map(LOCK_MAP_CAPACITY, 0, &treeZone->loadingPages);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return makeVIOPool(layer, BLOCK_MAP_VIO_POOL_SIZE, zone->threadID,
                     makeBlockMapVIOs, treeZone, &treeZone->vioPool);
}

/**********************************************************************/
int replaceTreeZoneVIOPool(struct block_map_tree_zone *zone,
                           PhysicalLayer              *layer,
                           size_t                      poolSize)
{
  freeVIOPool(&zone->vioPool);
  return makeVIOPool(layer, poolSize, zone->mapZone->threadID,
                     makeBlockMapVIOs, zone, &zone->vioPool);
}

/**********************************************************************/
void uninitializeBlockMapTreeZone(struct block_map_tree_zone *treeZone)
{
  free_dirty_lists(&treeZone->dirtyLists);
  freeVIOPool(&treeZone->vioPool);
  free_int_map(&treeZone->loadingPages);
}

/**********************************************************************/
void setTreeZoneInitialPeriod(struct block_map_tree_zone *treeZone,
                              SequenceNumber              period)
{
  set_current_period(treeZone->dirtyLists, period);
}

/**
 * Get the block_map_tree_zone in which a data_vio is operating.
 *
 * @param dataVIO  The data_vio
 *
 * @return The block_map_tree_zone
 **/
__attribute__((warn_unused_result))
static inline struct block_map_tree_zone *
getBlockMapTreeZone(struct data_vio *dataVIO)
{
  return &(get_block_map_for_zone(dataVIO->logical.zone)->treeZone);
}

/**
 * Get the tree_page for a given lock. This will be the page referred to by the
 * lock's tree slot for the lock's current height.
 *
 * @param zone  The tree zone of the tree
 * @param lock  The lock describing the page to get
 *
 * @return The requested page
 **/
static inline struct tree_page *
getTreePage(const struct block_map_tree_zone *zone,
            const struct tree_lock           *lock)
{
  return get_tree_page_by_index(zone->mapZone->blockMap->forest,
                                lock->rootIndex,
                                lock->height,
                                lock->treeSlots[lock->height].pageIndex);
}

/**********************************************************************/
bool copyValidPage(char                     *buffer,
                   Nonce                     nonce,
                   PhysicalBlockNumber       pbn,
                   struct block_map_page    *page)
{
  struct block_map_page *loaded   = (struct block_map_page *) buffer;
  BlockMapPageValidity   validity = validateBlockMapPage(loaded, nonce, pbn);
  if (validity == BLOCK_MAP_PAGE_VALID) {
    memcpy(page, loaded, VDO_BLOCK_SIZE);
    return true;
  }

  if (validity == BLOCK_MAP_PAGE_BAD) {
    logErrorWithStringError(VDO_BAD_PAGE,
                            "Expected page %" PRIu64
                            " but got page %llu instead",
                            pbn, getBlockMapPagePBN(loaded));
  }

  return false;
}

/**
 * Check whether the zone has any outstanding I/O, and if not, drain the page
 * cache.
 *
 * @param zone  The zone to check
 **/
static void checkForIOComplete(struct block_map_tree_zone *zone)
{
  if (is_draining(&zone->mapZone->state) && (zone->activeLookups == 0)
      && !hasWaiters(&zone->flushWaiters) && !isVIOPoolBusy(zone->vioPool)) {
    drainVDOPageCache(zone->mapZone->pageCache);
  }
}

/**
 * Put the vdo in read-only mode and wake any vios waiting for a flush.
 *
 * @param zone    The zone
 * @param result  The error which is causing read-only mode
 **/
static void enterZoneReadOnlyMode(struct block_map_tree_zone *zone, int result)
{
  enterReadOnlyMode(zone->mapZone->readOnlyNotifier, result);

  // We are in read-only mode, so we won't ever write any page out. Just take
  // all waiters off the queue so the tree zone can be closed.
  while (hasWaiters(&zone->flushWaiters)) {
    dequeueNextWaiter(&zone->flushWaiters);
  }

  checkForIOComplete(zone);
}

/**
 * Check whether a generation is strictly older than some other generation in
 * the context of a zone's current generation range.
 *
 * @param zone  The zone in which to do the comparison
 * @param a     The generation in question
 * @param b     The generation to compare to
 *
 * @return <code>true</code> if generation a is not strictly older than
 *         generation b in the context of the zone
 **/
__attribute__((warn_unused_result))
static bool isNotOlder(struct block_map_tree_zone *zone, uint8_t a, uint8_t b)
{
  int result = ASSERT((inCyclicRange(zone->oldestGeneration, a,
                                     zone->generation, 1 << 8)
                       && inCyclicRange(zone->oldestGeneration, b,
                                        zone->generation, 1 << 8)),
                      "generation(s) %u, %u are out of range [%u, %u]",
                      a, b, zone->oldestGeneration, zone->generation);
  if (result != VDO_SUCCESS) {
    enterZoneReadOnlyMode(zone, result);
    return true;
  }

  return inCyclicRange(b, a, zone->generation, 1 << 8);
}

/**
 * Decrement the count for a generation and roll the oldest generation if there
 * are no longer any active pages in it.
 *
 * @param zone        The zone
 * @param generation  The generation to release
 **/
static void releaseGeneration(struct block_map_tree_zone *zone,
                              uint8_t                     generation)
{
  int result = ASSERT((zone->dirtyPageCounts[generation] > 0),
                      "dirty page count underflow for generation %u",
                      generation);
  if (result != VDO_SUCCESS) {
    enterZoneReadOnlyMode(zone, result);
    return;
  }

  zone->dirtyPageCounts[generation]--;
  while ((zone->dirtyPageCounts[zone->oldestGeneration] == 0)
         && (zone->oldestGeneration != zone->generation)) {
    zone->oldestGeneration++;
  }
}

/**
 * Set the generation of a page and update the dirty page count in the zone.
 *
 * @param zone           The zone which owns the page
 * @param page           The page
 * @param newGeneration  The generation to set
 * @param decrementOld   Whether to decrement the count of the page's old
 *                       generation
 **/
static void setGeneration(struct block_map_tree_zone *zone,
                          struct tree_page           *page,
                          uint8_t                     newGeneration,
                          bool                        decrementOld)
{
  uint8_t oldGeneration = page->generation;
  if (decrementOld && (oldGeneration == newGeneration)) {
    return;
  }

  page->generation = newGeneration;
  uint32_t newCount = ++zone->dirtyPageCounts[newGeneration];
  int result = ASSERT((newCount != 0),
                      "dirty page count overflow for generation %u",
                      newGeneration);
  if (result != VDO_SUCCESS) {
    enterZoneReadOnlyMode(zone, result);
    return;
  }

  if (decrementOld) {
    releaseGeneration(zone, oldGeneration);
  }
}

/**********************************************************************/
static void writePage(struct tree_page *treePage, struct vio_pool_entry *entry);

/**
 * Write out a dirty page if it is still covered by the most recent flush
 * or if it is the flusher.
 *
 * <p>Implements WaiterCallback
 *
 * @param waiter   The page to write
 * @param context  The vio_pool_entry with which to do the write
 **/
static void writePageCallback(struct waiter *waiter, void *context)
{
  STATIC_ASSERT(offsetof(struct tree_page, waiter) == 0);
  writePage((struct tree_page *) waiter, (struct vio_pool_entry *) context);
}

/**
 * Acquire a vio for writing a dirty page.
 *
 * @param waiter  The page which needs a vio
 * @param zone    The zone
 **/
static void acquire_vio(struct waiter *waiter, struct block_map_tree_zone *zone)
{
  waiter->callback = writePageCallback;
  int result = acquireVIOFromPool(zone->vioPool, waiter);
  if (result != VDO_SUCCESS) {
    enterZoneReadOnlyMode(zone, result);
  }
}

/**
 * Attempt to increment the generation.
 *
 * @param zone  The zone whose generation is to be incremented
 *
 * @return <code>true</code> if all possible generations were not already
 *         active
 **/
static bool attemptIncrement(struct block_map_tree_zone *zone)
{
  uint8_t generation = zone->generation + 1;
  if (zone->oldestGeneration == generation) {
    return false;
  }

  zone->generation = generation;
  return true;
}

/**
 * Enqueue a page to either launch a flush or wait for the current flush which
 * is already in progress.
 *
 * @param page  The page to enqueue
 * @param zone  The zone
 **/
static void enqueuePage(struct tree_page           *page,
                        struct block_map_tree_zone *zone)
{
  if ((zone->flusher == NULL) && attemptIncrement(zone)) {
    zone->flusher = page;
    acquire_vio(&page->waiter, zone);
    return;
  }

  int result = enqueueWaiter(&zone->flushWaiters, &page->waiter);
  if (result != VDO_SUCCESS) {
    enterZoneReadOnlyMode(zone, result);
  }
}

/**
 * Write pages which were waiting for a flush and have not been redirtied.
 * Requeue those pages which were redirtied.
 *
 * <p>Implements WaiterCallback.
 *
 * @param waiter   The dirty page
 * @param context  The zone and generation
 **/
static void writePageIfNotDirtied(struct waiter *waiter, void *context)
{
  STATIC_ASSERT(offsetof(struct tree_page, waiter) == 0);
  struct tree_page *page = (struct tree_page *) waiter;
  struct write_if_not_dirtied_context *writeContext = context;
  if (page->generation == writeContext->generation) {
    acquire_vio(waiter, writeContext->zone);
    return;
  }

  enqueuePage(page, writeContext->zone);
}

/**
 * Return a vio to the zone's pool.
 *
 * @param zone   The zone which owns the pool
 * @param entry  The pool entry to return
 **/
static void returnToPool(struct block_map_tree_zone *zone,
                         struct vio_pool_entry      *entry)
{
  returnVIOToPool(zone->vioPool, entry);
  checkForIOComplete(zone);
}

/**
 * Handle the successful write of a tree page. This callback is registered in
 * writeInitializedPage().
 *
 * @param completion  The vio doing the write
 **/
static void finishPageWrite(struct vdo_completion *completion)
{
  struct vio_pool_entry               *entry = completion->parent;
  struct tree_page                    *page  = entry->parent;
  struct block_map_tree_zone          *zone  = entry->context;
  releaseRecoveryJournalBlockReference(zone->mapZone->blockMap->journal,
                                       page->writingRecoveryLock,
                                       ZONE_TYPE_LOGICAL,
                                       zone->mapZone->zoneNumber);

  bool dirty    = (page->writingGeneration != page->generation);
  releaseGeneration(zone, page->writingGeneration);
  page->writing = false;

  if (zone->flusher == page) {
    struct write_if_not_dirtied_context context = {
      .zone       = zone,
      .generation = page->writingGeneration,
    };
    notifyAllWaiters(&zone->flushWaiters, writePageIfNotDirtied, &context);
    if (dirty && attemptIncrement(zone)) {
      writePage(page, entry);
      return;
    }

    zone->flusher = NULL;
  }

  if (dirty) {
    enqueuePage(page, zone);
  } else if ((zone->flusher == NULL)
             && hasWaiters(&zone->flushWaiters)
             && attemptIncrement(zone)) {
    zone->flusher = (struct tree_page *) dequeueNextWaiter(&zone->flushWaiters);
    writePage(zone->flusher, entry);
    return;
  }

  returnToPool(zone, entry);
}

/**
 * Handle an error writing a tree page. This error handler is registered in
 * writePage() and writeInitializedPage().
 *
 * @param completion  The vio doing the write
 **/
static void handleWriteError(struct vdo_completion *completion)
{
  int                                  result = completion->result;
  struct vio_pool_entry               *entry  = completion->parent;
  struct block_map_tree_zone          *zone   = entry->context;
  enterZoneReadOnlyMode(zone, result);
  returnToPool(zone, entry);
}

/**
 * Write a page which has been written at least once. This callback is
 * registered in (or called directly from) writePage().
 *
 * @param completion  The vio which will do the write
 **/
static void writeInitializedPage(struct vdo_completion *completion)
{
  struct vio_pool_entry      *entry    = completion->parent;
  struct block_map_tree_zone *zone     =
    (struct block_map_tree_zone *) entry->context;
  struct tree_page           *treePage = (struct tree_page *) entry->parent;

  /*
   * Set the initialized field of the copy of the page we are writing to true.
   * We don't want to set it true on the real page in memory until after this
   * write succeeds.
   */
  struct block_map_page *page = (struct block_map_page *) entry->buffer;
  markBlockMapPageInitialized(page, true);
  launchWriteMetadataVIOWithFlush(entry->vio, getBlockMapPagePBN(page),
                                  finishPageWrite, handleWriteError,
                                  (zone->flusher == treePage), false);
}

/**
 * Write a dirty tree page now that we have a vio with which to write it.
 *
 * @param treePage  The page to write
 * @param entry     The vio_pool_entry with which to write
 **/
static void writePage(struct tree_page *treePage, struct vio_pool_entry *entry)
{
  struct block_map_tree_zone *zone
    = (struct block_map_tree_zone *) entry->context;
  if ((zone->flusher != treePage)
      && (isNotOlder(zone, treePage->generation, zone->generation))) {
    // This page was re-dirtied after the last flush was  issued, hence we need
    // to do another flush.
    enqueuePage(treePage, zone);
    returnToPool(zone, entry);
    return;
  }

  entry->parent = treePage;
  memcpy(entry->buffer, treePage->pageBuffer, VDO_BLOCK_SIZE);

  struct vdo_completion *completion = vioAsCompletion(entry->vio);
  completion->callbackThreadID      = zone->mapZone->threadID;

  treePage->writing             = true;
  treePage->writingGeneration   = treePage->generation;
  treePage->writingRecoveryLock = treePage->recoveryLock;

  // Clear this now so that we know this page is not on any dirty list.
  treePage->recoveryLock = 0;

  struct block_map_page *page = asBlockMapPage(treePage);
  if (!markBlockMapPageInitialized(page, true)) {
    writeInitializedPage(completion);
    return;
  }

  launchWriteMetadataVIO(entry->vio, getBlockMapPagePBN(page),
                         writeInitializedPage, handleWriteError);
}

/**
 * Schedule a batch of dirty pages for writing.
 *
 * <p>Implements DirtyListsCallback.
 *
 * @param expired  The pages to write
 * @param context  The zone
 **/
static void writeDirtyPagesCallback(RingNode *expired, void *context)
{
  struct block_map_tree_zone *zone
    = (struct block_map_tree_zone *) context;
  uint8_t generation = zone->generation;
  while (!isRingEmpty(expired)) {
    struct tree_page *page = treePageFromRingNode(chopRingNode(expired));

    int result = ASSERT(!isWaiting(&page->waiter),
                        "Newly expired page not already waiting to write");
    if (result != VDO_SUCCESS) {
      enterZoneReadOnlyMode(zone, result);
      continue;
    }

    setGeneration(zone, page, generation, false);
    if (!page->writing) {
      enqueuePage(page, zone);
    }
  }
}

/**********************************************************************/
void advanceZoneTreePeriod(struct block_map_tree_zone *zone,
                           SequenceNumber              period)
{
  advance_period(zone->dirtyLists, period);
}

/**********************************************************************/
void drainZoneTrees(struct admin_state *state)
{
  struct block_map_tree_zone *zone
    = &(container_of(state, struct block_map_zone, state)->treeZone);
  ASSERT_LOG_ONLY((zone->activeLookups == 0),
                  "drainZoneTrees() called with no active lookups");
  if (!is_suspending(state)) {
    flush_dirty_lists(zone->dirtyLists);
  }

  checkForIOComplete(zone);
}

/**
 * Release a lock on a page which was being loaded or allocated.
 *
 * @param dataVIO  The data_vio releasing the page lock
 * @param what     What the data_vio was doing (for logging)
 **/
static void releasePageLock(struct data_vio *dataVIO, char *what)
{
  struct tree_lock *lock = &dataVIO->treeLock;
  ASSERT_LOG_ONLY(lock->locked,
                  "release of unlocked block map page %s for key %" PRIu64
                  " in tree %u",
                  what, lock->key, lock->rootIndex);
  struct block_map_tree_zone *zone       = getBlockMapTreeZone(dataVIO);
  struct tree_lock           *lockHolder = int_map_remove(zone->loadingPages,
                                                          lock->key);
  ASSERT_LOG_ONLY((lockHolder == lock),
                  "block map page %s mismatch for key %llu in tree %u",
                  what, lock->key, lock->rootIndex);
  lock->locked = false;
}

/**
 * Continue a data_vio now that the lookup is complete.
 *
 * @param dataVIO  The data_vio
 * @param result   The result of the lookup
 **/
static void finishLookup(struct data_vio *dataVIO, int result)
{
  dataVIO->treeLock.height = 0;

  struct block_map_tree_zone *zone       = getBlockMapTreeZone(dataVIO);
  struct vdo_completion      *completion = dataVIOAsCompletion(dataVIO);
  setCompletionResult(completion, result);
  launchCallback(completion, dataVIO->treeLock.callback,
                 dataVIO->treeLock.threadID);
  --zone->activeLookups;
}

/**
 * Abort a block map PBN lookup due to an error in the load or allocation on
 * which we were waiting.
 *
 * @param waiter   The data_vio which was waiting for a page load or allocation
 * @param context  The error which caused the abort
 **/
static void abortLookupForWaiter(struct waiter *waiter, void *context)
{
  struct data_vio *dataVIO = waiterAsDataVIO(waiter);
  int      result  = *((int *) context);
  if (isReadDataVIO(dataVIO)) {
    if (result == VDO_NO_SPACE) {
      result = VDO_SUCCESS;
    }
  } else if (result != VDO_NO_SPACE) {
    result = VDO_READ_ONLY;
  }

  finishLookup(dataVIO, result);
}

/**
 * Abort a block map PBN lookup due to an error loading or allocating a page.
 *
 * @param dataVIO  The data_vio which was loading or allocating a page
 * @param result   The error code
 * @param what     What the data_vio was doing (for logging)
 **/
static void abortLookup(struct data_vio *dataVIO, int result, char *what)
{
  if (result != VDO_NO_SPACE) {
    enterZoneReadOnlyMode(getBlockMapTreeZone(dataVIO), result);
  }

  if (dataVIO->treeLock.locked) {
    releasePageLock(dataVIO, what);
    notifyAllWaiters(&dataVIO->treeLock.waiters, abortLookupForWaiter,
                     &result);
  }

  finishLookup(dataVIO, result);
}

/**
 * Abort a block map PBN lookup due to an error loading a page.
 *
 * @param dataVIO  The data_vio doing the page load
 * @param result   The error code
 **/
static void abortLoad(struct data_vio *dataVIO, int result)
{
  abortLookup(dataVIO, result, "load");
}

/**
 * Determine if a location represents a valid mapping for a tree page.
 *
 * @param vdo      The vdo
 * @param mapping  The data_location to check
 * @param height   The height of the entry in the tree
 *
 * @return <code>true</code> if the entry represents a invalid page mapping
 **/
__attribute__((warn_unused_result))
static bool isInvalidTreeEntry(const struct vdo           *vdo,
                               const struct data_location *mapping,
                               Height                      height)
{
  if (!isValidLocation(mapping)
      || isCompressed(mapping->state)
      || (isMappedLocation(mapping) && (mapping->pbn == ZERO_BLOCK))) {
    return true;
  }

  // Roots aren't physical data blocks, so we can't check their PBNs.
  if (height == BLOCK_MAP_TREE_HEIGHT) {
    return false;
  }

  return !isPhysicalDataBlock(vdo->depot, mapping->pbn);
}

/**********************************************************************/
static void loadBlockMapPage(struct block_map_tree_zone *zone,
                             struct data_vio            *dataVIO);

static void allocateBlockMapPage(struct block_map_tree_zone *zone,
                                 struct data_vio            *dataVIO);

/**
 * Continue a block map PBN lookup now that a page has been loaded by
 * descending one level in the tree.
 *
 * @param dataVIO  The data_vio doing the lookup
 * @param page     The page which was just loaded
 **/
static void continueWithLoadedPage(struct data_vio       *dataVIO,
                                   struct block_map_page *page)
{
  struct tree_lock *lock = &dataVIO->treeLock;
  struct block_map_tree_slot  slot = lock->treeSlots[lock->height];
  struct data_location mapping
    = unpackBlockMapEntry(&page->entries[slot.blockMapSlot.slot]);
  if (isInvalidTreeEntry(getVDOFromDataVIO(dataVIO), &mapping, lock->height)) {
    logErrorWithStringError(VDO_BAD_MAPPING,
                            "Invalid block map tree PBN: %llu with "
                            "state %u for page index %u at height %u",
                            mapping.pbn, mapping.state,
                            lock->treeSlots[lock->height - 1].pageIndex,
                            lock->height - 1);
    abortLoad(dataVIO, VDO_BAD_MAPPING);
    return;
  }

  if (!isMappedLocation(&mapping)) {
    // The page we need is unallocated
    allocateBlockMapPage(getBlockMapTreeZone(dataVIO), dataVIO);
    return;
  }

  lock->treeSlots[lock->height - 1].blockMapSlot.pbn = mapping.pbn;
  if (lock->height == 1) {
    finishLookup(dataVIO, VDO_SUCCESS);
    return;
  }

  // We know what page we need to load next
  loadBlockMapPage(getBlockMapTreeZone(dataVIO), dataVIO);
}

/**
 * Continue a block map PBN lookup now that the page load we were waiting on
 * has finished.
 *
 * @param waiter   The data_vio waiting for a page to be loaded
 * @param context  The page which was just loaded
 **/
static void continueLoadForWaiter(struct waiter *waiter, void *context)
{
  struct data_vio *dataVIO = waiterAsDataVIO(waiter);
  dataVIO->treeLock.height--;
  continueWithLoadedPage(dataVIO, (struct block_map_page *) context);
}

/**
 * Finish loading a page now that it has been read in from disk. This callback
 * is registered in loadPage().
 *
 * @param completion  The vio doing the page read
 **/
static void finishBlockMapPageLoad(struct vdo_completion *completion)
{
  struct vio_pool_entry     *entry    = completion->parent;
  struct data_vio           *dataVIO  = entry->parent;
  struct block_map_tree_zone *zone
    = (struct block_map_tree_zone *) entry->context;
  struct tree_lock          *treeLock = &dataVIO->treeLock;

  treeLock->height--;
  PhysicalBlockNumber pbn
    = treeLock->treeSlots[treeLock->height].blockMapSlot.pbn;
  struct tree_page *treePage = getTreePage(zone, treeLock);
  struct block_map_page *page
    = (struct block_map_page *) treePage->pageBuffer;
  Nonce         nonce    = zone->mapZone->blockMap->nonce;
  if (!copyValidPage(entry->buffer, nonce, pbn, page)) {
    formatBlockMapPage(page, nonce, pbn, false);
  }
  returnVIOToPool(zone->vioPool, entry);

  // Release our claim to the load and wake any waiters
  releasePageLock(dataVIO, "load");
  notifyAllWaiters(&treeLock->waiters, continueLoadForWaiter, page);
  continueWithLoadedPage(dataVIO, page);
}

/**
 * Handle an error loading a tree page.
 *
 * @param completion  The vio doing the page read
 **/
static void handleIOError(struct vdo_completion *completion)
{
  int                        result  = completion->result;
  struct vio_pool_entry     *entry   = completion->parent;
  struct data_vio           *dataVIO = entry->parent;
  struct block_map_tree_zone *zone
    = (struct block_map_tree_zone *) entry->context;
  returnVIOToPool(zone->vioPool, entry);
  abortLoad(dataVIO, result);
}

/**
 * Read a tree page from disk now that we've gotten a vio with which to do the
 * read. This WaiterCallback is registered in loadBlockMapPage().
 *
 * @param waiter   The data_vio which requires a page load
 * @param context  The VIOPool entry with which to do the read
 **/
static void loadPage(struct waiter *waiter, void *context)
{
  struct vio_pool_entry *entry   = context;
  struct data_vio       *dataVIO = waiterAsDataVIO(waiter);

  entry->parent = dataVIO;
  entry->vio->completion.callbackThreadID
    = get_block_map_for_zone(dataVIO->logical.zone)->threadID;

  struct tree_lock *lock = &dataVIO->treeLock;
  launchReadMetadataVIO(entry->vio,
                        lock->treeSlots[lock->height - 1].blockMapSlot.pbn,
                        finishBlockMapPageLoad, handleIOError);
}

/**
 * Attempt to acquire a lock on a page in the block map tree. If the page is
 * already locked, queue up to wait for the lock to be released. If the lock is
 * acquired, the data_vio's treeLock.locked field will be set to true.
 *
 * @param zone     The block_map_tree_zone in which the data_vio operates
 * @param dataVIO  The data_vio which desires a page lock
 *
 * @return VDO_SUCCESS or an error
 **/
static int attemptPageLock(struct block_map_tree_zone *zone,
                           struct data_vio            *dataVIO)
{
  struct tree_lock *lock              = &dataVIO->treeLock;
  Height            height            = lock->height;
  struct block_map_tree_slot treeSlot = lock->treeSlots[height];
  PageKey           key;
  key.descriptor = (struct page_descriptor) {
    .rootIndex = lock->rootIndex,
    .height    = height,
    .pageIndex = treeSlot.pageIndex,
    .slot      = treeSlot.blockMapSlot.slot,
  };
  lock->key = key.key;

  struct tree_lock *lockHolder;
  int result = int_map_put(zone->loadingPages, lock->key, lock, false,
                           (void **) &lockHolder);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (lockHolder == NULL) {
    // We got the lock
    dataVIO->treeLock.locked = true;
    return VDO_SUCCESS;
  }

  // Someone else is loading or allocating the page we need
  return enqueueDataVIO(&lockHolder->waiters, dataVIO,
                        THIS_LOCATION("$F;cb=blockMapTreePage"));
}

/**
 * Load a block map tree page from disk.
 *
 * @param zone     The block_map_tree_zone in which the data_vio operates
 * @param dataVIO  The data_vio which requires a page to be loaded
 **/
static void loadBlockMapPage(struct block_map_tree_zone *zone,
                             struct data_vio            *dataVIO)
{
  int result = attemptPageLock(zone, dataVIO);
  if (result != VDO_SUCCESS) {
    abortLoad(dataVIO, result);
    return;
  }

  if (dataVIO->treeLock.locked) {
    struct waiter *waiter = dataVIOAsWaiter(dataVIO);
    waiter->callback = loadPage;
    result = acquireVIOFromPool(zone->vioPool, waiter);
    if (result != VDO_SUCCESS) {
      abortLoad(dataVIO, result);
    }
  }
}

/**
 * Set the callback of a data_vio after it has allocated a block map page.
 *
 * @param dataVIO  The data_vio
 **/
static void setPostAllocationCallback(struct data_vio *dataVIO)
{
  setCallback(dataVIOAsCompletion(dataVIO), dataVIO->treeLock.callback,
              dataVIO->treeLock.threadID);
}

/**
 * Abort a block map PBN lookup due to an error allocating a page.
 *
 * @param dataVIO  The data_vio doing the page allocation
 * @param result   The error code
 **/
static void abortAllocation(struct data_vio *dataVIO, int result)
{
  setPostAllocationCallback(dataVIO);
  abortLookup(dataVIO, result, "allocation");
}

/**
 * Callback to handle an error while attempting to allocate a page. This
 * callback is used to transfer back to the logical zone along the block map
 * page allocation path.
 *
 * @param completion  The data_vio doing the allocation
 **/
static void allocationFailure(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  abortAllocation(dataVIO, completion->result);
}

/**
 * Continue with page allocations now that a parent page has been allocated.
 *
 * @param waiter   The data_vio which was waiting for a page to be allocated
 * @param context  The physical block number of the page which was just
 *                 allocated
 **/
static void continueAllocationForWaiter(struct waiter *waiter, void *context)
{
  struct data_vio     *dataVIO  = waiterAsDataVIO(waiter);
  struct tree_lock    *treeLock = &dataVIO->treeLock;
  PhysicalBlockNumber  pbn      = *((PhysicalBlockNumber *) context);

  treeLock->height--;
  dataVIO->treeLock.treeSlots[treeLock->height].blockMapSlot.pbn = pbn;

  if (treeLock->height == 0) {
    finishLookup(dataVIO, VDO_SUCCESS);
    return;
  }

  allocateBlockMapPage(getBlockMapTreeZone(dataVIO), dataVIO);
}

/**
 * Finish the page allocation process by recording the allocation in the tree
 * and waking any waiters now that the write lock has been released. This
 * callback is registered in releaseBlockMapWriteLock().
 *
 * @param completion  The data_vio doing the allocation
 **/
static void finishBlockMapAllocation(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  if (completion->result != VDO_SUCCESS) {
    allocationFailure(completion);
    return;
  }

  struct block_map_tree_zone *zone     = getBlockMapTreeZone(dataVIO);
  struct tree_lock *treeLock           = &dataVIO->treeLock;
  struct tree_page *treePage           = getTreePage(zone, treeLock);
  Height            height             = treeLock->height;

  PhysicalBlockNumber pbn = treeLock->treeSlots[height - 1].blockMapSlot.pbn;

  // Record the allocation.
  struct block_map_page *page
    = (struct block_map_page *) treePage->pageBuffer;
  SequenceNumber oldLock = treePage->recoveryLock;
  updateBlockMapPage(page, dataVIO, pbn, MAPPING_STATE_UNCOMPRESSED,
                     &treePage->recoveryLock);

  if (isWaiting(&treePage->waiter)) {
    // This page is waiting to be written out.
    if (zone->flusher != treePage) {
      // The outstanding flush won't cover the update we just made, so mark
      // the page as needing another flush.
      setGeneration(zone, treePage, zone->generation, true);
    }
  } else {
    // Put the page on a dirty list
    if (oldLock == 0) {
      initializeRing(&treePage->node);
    }
    add_to_dirty_lists(zone->dirtyLists, &treePage->node, oldLock,
                       treePage->recoveryLock);
  }

  treeLock->height--;
  if (height > 1) {
    // Format the interior node we just allocated (in memory).
    treePage = getTreePage(zone, treeLock);
    formatBlockMapPage(treePage->pageBuffer, zone->mapZone->blockMap->nonce,
                       pbn, false);
  }

  // Release our claim to the allocation and wake any waiters
  releasePageLock(dataVIO, "allocation");
  notifyAllWaiters(&treeLock->waiters, continueAllocationForWaiter, &pbn);
  if (treeLock->height == 0) {
    finishLookup(dataVIO, VDO_SUCCESS);
    return;
  }

  allocateBlockMapPage(zone, dataVIO);
}

/**
 * Release the write lock on a newly allocated block map page now that we
 * have made its journal entries and reference count updates. This callback
 * is registered in setBlockMapPageReferenceCount().
 *
 * @param completion  The data_vio doing the allocation
 **/
static void releaseBlockMapWriteLock(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  struct allocating_vio *allocatingVIO = dataVIOAsAllocatingVIO(dataVIO);
  assertInAllocatedZone(dataVIO);
  if (completion->result != VDO_SUCCESS) {
    launchLogicalCallback(dataVIO, allocationFailure, THIS_LOCATION(NULL));
    return;
  }

  release_allocation_lock(allocatingVIO);
  reset_allocation(allocatingVIO);
  launchLogicalCallback(dataVIO, finishBlockMapAllocation,
                        THIS_LOCATION("$F;cb=finishBlockMapAllocation"));
}

/**
 * Set the reference count of a newly allocated block map page to
 * MAXIMUM_REFERENCES now that we have made a recovery journal entry for it.
 * MAXIMUM_REFERENCES is used to prevent deduplication against the block after
 * we release the write lock on it, but before we write out the page.
 *
 * @param completion  The data_vio doing the allocation
 **/
static void setBlockMapPageReferenceCount(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  assertInAllocatedZone(dataVIO);
  if (completion->result != VDO_SUCCESS) {
    launchLogicalCallback(dataVIO, allocationFailure, THIS_LOCATION(NULL));
    return;
  }

  struct tree_lock *lock = &dataVIO->treeLock;
  PhysicalBlockNumber pbn = lock->treeSlots[lock->height - 1].blockMapSlot.pbn;
  completion->callback = releaseBlockMapWriteLock;
  addSlabJournalEntry(getSlabJournal(getVDOFromDataVIO(dataVIO)->depot, pbn),
                      dataVIO);
}

/**
 * Make a recovery journal entry for a newly allocated block map page.
 * This callback is registered in continueBlockMapPageAllocation().
 *
 * @param completion  The data_vio doing the allocation
 **/
static void journalBlockMapAllocation(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  assertInJournalZone(dataVIO);
  if (completion->result != VDO_SUCCESS) {
    launchLogicalCallback(dataVIO, allocationFailure, THIS_LOCATION(NULL));
    return;
  }

  setAllocatedZoneCallback(dataVIO, setBlockMapPageReferenceCount,
                           THIS_LOCATION(NULL));
  addRecoveryJournalEntry(getVDOFromDataVIO(dataVIO)->recoveryJournal,
                          dataVIO);
}

/**
 * Continue the process of allocating a block map page now that the
 * BlockAllocator has given us a block. This method is supplied as the callback
 * to allocate_data_block() by allocateBlockMapPage().
 *
 * @param allocatingVIO  The data_vio which is doing the allocation
 **/
static void continueBlockMapPageAllocation(struct allocating_vio *allocatingVIO)
{
  struct data_vio *dataVIO = allocatingVIOAsDataVIO(allocatingVIO);
  if (!hasAllocation(dataVIO)) {
    setLogicalCallback(dataVIO, allocationFailure, THIS_LOCATION(NULL));
    continueDataVIO(dataVIO, VDO_NO_SPACE);
    return;
  }

  PhysicalBlockNumber  pbn  = allocatingVIO->allocation;
  struct tree_lock    *lock = &dataVIO->treeLock;
  lock->treeSlots[lock->height - 1].blockMapSlot.pbn = pbn;
  setUpReferenceOperationWithLock(BLOCK_MAP_INCREMENT, pbn,
                                  MAPPING_STATE_UNCOMPRESSED,
                                  allocatingVIO->allocation_lock,
                                  &dataVIO->operation);
  launchJournalCallback(dataVIO, journalBlockMapAllocation,
                        THIS_LOCATION("$F;cb=journalBlockMapAllocation"));
}

/**
 * Allocate a block map page.
 *
 * @param zone     The zone in which the data_vio is operating
 * @param dataVIO  The data_vio which needs to allocate a page
 **/
static void allocateBlockMapPage(struct block_map_tree_zone *zone,
                                 struct data_vio            *dataVIO)
{
  if (!isWriteDataVIO(dataVIO) || isTrimDataVIO(dataVIO)) {
    // This is a pure read, the read phase of a read-modify-write, or a trim,
    // so there's nothing left to do here.
    finishLookup(dataVIO, VDO_SUCCESS);
    return;
  }

  int result = attemptPageLock(zone, dataVIO);
  if (result != VDO_SUCCESS) {
    abortAllocation(dataVIO, result);
    return;
  }

  if (!dataVIO->treeLock.locked) {
    return;
  }

  allocate_data_block(dataVIOAsAllocatingVIO(dataVIO),
                      get_allocation_selector(dataVIO->logical.zone),
                      VIO_BLOCK_MAP_WRITE_LOCK,
                      continueBlockMapPageAllocation);
}

/**********************************************************************/
void lookupBlockMapPBN(struct data_vio *dataVIO)
{
  struct block_map_tree_zone *zone = getBlockMapTreeZone(dataVIO);
  zone->activeLookups++;
  if (is_draining(&zone->mapZone->state)) {
    finishLookup(dataVIO, VDO_SHUTTING_DOWN);
    return;
  }

  struct tree_lock *lock = &dataVIO->treeLock;
  PageNumber pageIndex
    = ((lock->treeSlots[0].pageIndex - zone->mapZone->blockMap->flatPageCount)
       / zone->mapZone->blockMap->rootCount);
  struct block_map_tree_slot treeSlot = {
    .pageIndex = pageIndex / BLOCK_MAP_ENTRIES_PER_PAGE,
    .blockMapSlot = {
      .pbn  = 0,
      .slot = pageIndex % BLOCK_MAP_ENTRIES_PER_PAGE,
    },
  };

  struct block_map_page *page = NULL;
  for (lock->height = 1; lock->height <= BLOCK_MAP_TREE_HEIGHT;
       lock->height++) {
    lock->treeSlots[lock->height] = treeSlot;
    page = (struct block_map_page *) (getTreePage(zone, lock)->pageBuffer);
    PhysicalBlockNumber pbn = getBlockMapPagePBN(page);
    if (pbn != ZERO_BLOCK) {
      lock->treeSlots[lock->height].blockMapSlot.pbn = pbn;
      break;
    }

    // Calculate the index and slot for the next level.
    treeSlot.blockMapSlot.slot
      = treeSlot.pageIndex % BLOCK_MAP_ENTRIES_PER_PAGE;
    treeSlot.pageIndex
      = treeSlot.pageIndex / BLOCK_MAP_ENTRIES_PER_PAGE;
  }

  // The page at this height has been allocated and loaded.
  struct data_location mapping
    = unpackBlockMapEntry(&page->entries[treeSlot.blockMapSlot.slot]);
  if (isInvalidTreeEntry(getVDOFromDataVIO(dataVIO), &mapping, lock->height)) {
    logErrorWithStringError(VDO_BAD_MAPPING,
                            "Invalid block map tree PBN: %llu with "
                            "state %u for page index %u at height %u",
                            mapping.pbn, mapping.state,
                            lock->treeSlots[lock->height - 1].pageIndex,
                            lock->height - 1);
    abortLoad(dataVIO, VDO_BAD_MAPPING);
    return;
  }

  if (!isMappedLocation(&mapping)) {
    // The page we want one level down has not been allocated, so allocate it.
    allocateBlockMapPage(zone, dataVIO);
    return;
  }

  lock->treeSlots[lock->height - 1].blockMapSlot.pbn = mapping.pbn;
  if (lock->height == 1) {
    // This is the ultimate block map page, so we're done
    finishLookup(dataVIO, VDO_SUCCESS);
    return;
  }

  // We know what page we need to load.
  loadBlockMapPage(zone, dataVIO);
}

/**********************************************************************/
PhysicalBlockNumber findBlockMapPagePBN(struct block_map *map,
                                        PageNumber        pageNumber)
{
  if (pageNumber < map->flatPageCount) {
    return (BLOCK_MAP_FLAT_PAGE_ORIGIN + pageNumber);
  }

  RootCount  rootIndex = pageNumber % map->rootCount;
  PageNumber pageIndex = ((pageNumber - map->flatPageCount) / map->rootCount);
  SlotNumber slot      = pageIndex % BLOCK_MAP_ENTRIES_PER_PAGE;
  pageIndex /= BLOCK_MAP_ENTRIES_PER_PAGE;

  struct tree_page *treePage
    = get_tree_page_by_index(map->forest, rootIndex, 1, pageIndex);
  struct block_map_page *page = (struct block_map_page *) treePage->pageBuffer;
  if (!isBlockMapPageInitialized(page)) {
    return ZERO_BLOCK;
  }

  struct data_location mapping = unpackBlockMapEntry(&page->entries[slot]);
  if (!isValidLocation(&mapping) || isCompressed(mapping.state)) {
    return ZERO_BLOCK;
  }
  return mapping.pbn;
}

/**********************************************************************/
void writeTreePage(struct tree_page *page, struct block_map_tree_zone *zone)
{
  bool waiting = isWaiting(&page->waiter);
  if (waiting && (zone->flusher == page)) {
    return;
  }

  setGeneration(zone, page, zone->generation, waiting);
  if (waiting || page->writing) {
    return;
  }

  enqueuePage(page, zone);
}
