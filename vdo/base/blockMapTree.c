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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMapTree.c#9 $
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
#include "vioPool.h"

enum {
  BLOCK_MAP_VIO_POOL_SIZE = 64,
};

typedef struct __attribute__((packed)) {
  RootCount  rootIndex;
  Height     height;
  PageNumber pageIndex;
  SlotNumber slot;
} PageDescriptor;

typedef union {
  PageDescriptor descriptor;
  uint64_t       key;
} PageKey;

typedef struct {
  BlockMapTreeZone *zone;
  uint8_t           generation;
} WriteIfNotDirtiedContext;

/**
 * An invalid PBN used to indicate that the page holding the location of a
 * tree root has been "loaded".
 **/
const PhysicalBlockNumber INVALID_PBN = 0xFFFFFFFFFFFFFFFF;

/**
 * Convert a RingNode to a TreePage.
 *
 * @param ringNode The RingNode to convert
 *
 * @return The TreePage which owns the RingNode
 **/
static inline TreePage *treePageFromRingNode(RingNode *ringNode)
{
  return (TreePage *) ((byte *) ringNode - offsetof(TreePage, node));
}

/**********************************************************************/
static void writeDirtyPagesCallback(RingNode *expired, void *context);

/**
 * Make VIOs for reading, writing, and allocating the arboreal block map.
 *
 * Implements VIOConstructor.
 **/
__attribute__((warn_unused_result))
static int makeBlockMapVIOs(PhysicalLayer  *layer,
                            void           *parent,
                            void           *buffer,
                            VIO           **vioPtr)
{
  return createVIO(layer, VIO_TYPE_BLOCK_MAP_INTERIOR, VIO_PRIORITY_METADATA,
                   parent, buffer, vioPtr);
}

/**********************************************************************/
int initializeTreeZone(BlockMapZone        *zone,
                       PhysicalLayer       *layer,
                       ReadOnlyModeContext *readOnlyContext,
                       BlockCount           eraLength)
{
  STATIC_ASSERT_SIZEOF(PageDescriptor, sizeof(uint64_t));
  BlockMapTreeZone *treeZone = &zone->treeZone;
  treeZone->mapZone          = zone;
  treeZone->readOnlyContext  = readOnlyContext;

  int result = makeDirtyLists(eraLength, writeDirtyPagesCallback, treeZone,
                              &treeZone->dirtyLists);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeIntMap(LOCK_MAP_CAPACITY, 0, &treeZone->loadingPages);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return makeVIOPool(layer, BLOCK_MAP_VIO_POOL_SIZE, makeBlockMapVIOs,
                     treeZone, &treeZone->vioPool);
}

/**********************************************************************/
int replaceTreeZoneVIOPool(BlockMapTreeZone *zone,
                           PhysicalLayer    *layer,
                           size_t            poolSize)
{
  freeVIOPool(&zone->vioPool);
  return makeVIOPool(layer, poolSize, makeBlockMapVIOs, zone, &zone->vioPool);
}

/**********************************************************************/
void uninitializeBlockMapTreeZone(BlockMapTreeZone *treeZone)
{
  freeDirtyLists(&treeZone->dirtyLists);
  freeVIOPool(&treeZone->vioPool);
  freeIntMap(&treeZone->loadingPages);
}

/**********************************************************************/
void setTreeZoneInitialPeriod(BlockMapTreeZone *treeZone,
                              SequenceNumber    period)
{
  setCurrentPeriod(treeZone->dirtyLists, period);
}

/**
 * Get the BlockMapTreeZone in which a DataVIO is operating.
 *
 * @param dataVIO  The DataVIO
 *
 * @return The BlockMapTreeZone
 **/
__attribute__((warn_unused_result))
static inline BlockMapTreeZone *getBlockMapTreeZone(DataVIO *dataVIO)
{
  return &(getBlockMapForZone(dataVIO->logical.zone)->treeZone);
}

/**
 * Get the BlockMapTree for the LBN of a DataVIO.
 *
 * @param zone     The BlockMapTreeZone in which the DataVIO is operating
 * @param dataVIO  The DataVIO
 *
 * @return The BlockMapTree for the DataVIO's LBN
 **/
static inline BlockMapTree *getTree(BlockMapTreeZone *zone, DataVIO *dataVIO)
{
  RootCount rootIndex = dataVIO->treeLock.rootIndex;
  return getTreeFromForest(zone->mapZone->blockMap->forest, rootIndex);
}

/**
 * Get the TreePage for a given lock. This will be the page referred to by the
 * lock's tree slot for the lock's current height.
 *
 * @param zone  The tree zone of the tree
 * @param tree  The BlockMapTree from which to get the page
 * @param lock  The lock describing the page to get
 *
 * @return The requested page
 **/
static inline TreePage *getTreePage(BlockMapTreeZone *zone,
                                    BlockMapTree     *tree,
                                    TreeLock         *lock)
{
  return getTreePageByIndex(zone->mapZone->blockMap->forest, tree,
                            lock->height,
                            lock->treeSlots[lock->height].pageIndex);
}

/**********************************************************************/
bool copyValidPage(char                *buffer,
                   Nonce                nonce,
                   PhysicalBlockNumber  pbn,
                   BlockMapPage        *page)
{
  BlockMapPage         *loaded   = (BlockMapPage *) buffer;
  BlockMapPageValidity  validity = validateBlockMapPage(loaded, nonce, pbn);
  if (validity == BLOCK_MAP_PAGE_VALID) {
    memcpy(page, loaded, VDO_BLOCK_SIZE);
    return true;
  }

  if (validity == BLOCK_MAP_PAGE_BAD) {
    logErrorWithStringError(VDO_BAD_PAGE,
                            "Expected page %" PRIu64
                            " but got page %" PRIu64 " instead",
                            pbn, getBlockMapPagePBN(loaded));
  }

  return false;
}

/**
 * Check whether the zone has any outstanding I/O, and if not, finish the zone
 * completion.
 *
 * @param zone  The zone to check
 **/
static void checkForIOComplete(BlockMapTreeZone *zone)
{
  if ((zone->mapZone->adminState != ADMIN_STATE_CLOSING)
      || (zone->activeLookups > 0) || hasWaiters(&zone->flushWaiters)) {
    return;
  }

  zone->mapZone->adminState = ADMIN_STATE_CLOSED;
  if (isReadOnly(zone->readOnlyContext)) {
    setCompletionResult(&zone->mapZone->completion, VDO_READ_ONLY);
  }
  closeObjectPool(zone->vioPool, &zone->mapZone->completion);
}

/**
 * Put the VDO in read-only mode and wake any VIOs waiting for a flush.
 *
 * @param zone    The zone
 * @param result  The error which is causing read-only mode
 **/
static void enterZoneReadOnlyMode(BlockMapTreeZone *zone, int result)
{
  enterReadOnlyMode(zone->readOnlyContext, result);

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
static bool isNotOlder(BlockMapTreeZone *zone, uint8_t a, uint8_t b)
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
static void releaseGeneration(BlockMapTreeZone *zone, uint8_t generation)
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
static void setGeneration(BlockMapTreeZone *zone,
                          TreePage         *page,
                          uint8_t           newGeneration,
                          bool              decrementOld)
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
static void writePage(TreePage *treePage, VIOPoolEntry *entry);

/**
 * Write out a dirty page if it is still covered by the most recent flush
 * or if it is the flusher.
 *
 * <p>Implements WaiterCallback
 *
 * @param waiter   The page to write
 * @param context  The VIOPoolEntry with which to do the write
 **/
static void writePageCallback(Waiter *waiter, void *context)
{
  STATIC_ASSERT(offsetof(TreePage, waiter) == 0);
  writePage((TreePage *) waiter, (VIOPoolEntry *) context);
}

/**
 * Acquire a VIO for writing a dirty page.
 *
 * @param waiter  The page which needs a VIO
 * @param zone    The zone
 **/
static void acquireVIO(Waiter *waiter, BlockMapTreeZone *zone)
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
static bool attemptIncrement(BlockMapTreeZone *zone)
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
static void enqueuePage(TreePage *page, BlockMapTreeZone *zone)
{
  if ((zone->flusher == NULL) && attemptIncrement(zone)) {
    zone->flusher = page;
    acquireVIO(&page->waiter, zone);
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
static void writePageIfNotDirtied(Waiter *waiter, void *context)
{
  STATIC_ASSERT(offsetof(TreePage, waiter) == 0);
  TreePage *page = (TreePage *) waiter;
  WriteIfNotDirtiedContext *writeContext = context;
  if (page->generation == writeContext->generation) {
    acquireVIO(waiter, writeContext->zone);
    return;
  }

  enqueuePage(page, writeContext->zone);
}

/**
 * Handle the successful write of a tree page. This callback is registered in
 * writeInitializedPage().
 *
 * @param completion  The VIO doing the write
 **/
static void finishPageWrite(VDOCompletion *completion)
{
  VIOPoolEntry     *entry = completion->parent;
  TreePage         *page  = entry->parent;
  BlockMapTreeZone *zone  = entry->context;
  releaseRecoveryJournalBlockReference(zone->mapZone->blockMap->journal,
                                       page->writingRecoveryLock,
                                       ZONE_TYPE_LOGICAL,
                                       zone->mapZone->zoneNumber);

  bool      dirty = (page->writingGeneration != page->generation);
  releaseGeneration(zone, page->writingGeneration);
  page->writing = false;

  if (zone->flusher == page) {
    WriteIfNotDirtiedContext context = {
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
    zone->flusher = (TreePage *) dequeueNextWaiter(&zone->flushWaiters);
    writePage(zone->flusher, entry);
    return;
  }

  checkForIOComplete(zone);
  returnVIOToPool(zone->vioPool, entry);
}

/**
 * Handle an error writing a tree page. This error handler is registered in
 * writePage() and writeInitializedPage().
 *
 * @param completion  The VIO doing the write
 **/
static void handleWriteError(VDOCompletion *completion)
{
  int               result = completion->result;
  VIOPoolEntry     *entry  = completion->parent;
  BlockMapTreeZone *zone   = entry->context;
  enterZoneReadOnlyMode(zone, result);
  returnVIOToPool(zone->vioPool, entry);
}

/**
 * Write a page which has been written at least once. This callback is
 * registered in (or called directly from) writePage().
 *
 * @param completion  The VIO which will do the write
 **/
static void writeInitializedPage(VDOCompletion *completion)
{
  VIOPoolEntry     *entry    = completion->parent;
  BlockMapTreeZone *zone     = (BlockMapTreeZone *) entry->context;
  TreePage         *treePage = (TreePage *) entry->parent;

  /*
   * Set the initialized field of the copy of the page we are writing to true.
   * We don't want to set it true on the real page in memory until after this
   * write succeeds.
   */
  BlockMapPage *page = (BlockMapPage *) entry->buffer;
  markBlockMapPageInitialized(page, true);
  launchWriteMetadataVIOWithFlush(entry->vio, getBlockMapPagePBN(page),
                                  finishPageWrite, handleWriteError,
                                  (zone->flusher == treePage), false);
}

/**
 * Write a dirty tree page now that we have a VIO with which to write it.
 *
 * @param treePage  The page to write
 * @param entry     The VIOPoolEntry with which to write
 **/
static void writePage(TreePage *treePage, VIOPoolEntry *entry)
{
  BlockMapTreeZone *zone = (BlockMapTreeZone *) entry->context;
  if ((zone->flusher != treePage)
      && (isNotOlder(zone, treePage->generation, zone->generation))) {
    // This page was re-dirtied after the last flush was issued, hence we need
    // to do another flush.
    enqueuePage(treePage, zone);
    returnVIOToPool(zone->vioPool, entry);
    return;
  }

  entry->parent = treePage;
  memcpy(entry->buffer, treePage->pageBuffer, VDO_BLOCK_SIZE);

  VDOCompletion *completion    = vioAsCompletion(entry->vio);
  completion->callbackThreadID = zone->mapZone->threadID;

  treePage->writing             = true;
  treePage->writingGeneration   = treePage->generation;
  treePage->writingRecoveryLock = treePage->recoveryLock;

  // Clear this now so that we know this page is not on any dirty list.
  treePage->recoveryLock = 0;

  BlockMapPage *page = asBlockMapPage(treePage);
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
  BlockMapTreeZone *zone       = (BlockMapTreeZone *) context;
  uint8_t           generation = zone->generation;
  while (!isRingEmpty(expired)) {
    TreePage *page = treePageFromRingNode(chopRingNode(expired));

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
void advanceZoneTreePeriod(BlockMapTreeZone *zone, SequenceNumber period)
{
  advancePeriod(zone->dirtyLists, period);
}

/**********************************************************************/
void closeZoneTrees(BlockMapTreeZone *zone)
{
  if (zone->mapZone->adminState == ADMIN_STATE_CLOSED) {
    finishCompletion(&zone->mapZone->completion, VDO_SUCCESS);
    return;
  }

  if (zone->activeLookups > 0) {
    zone->mapZone->adminState = ADMIN_STATE_CLOSE_REQUESTED;
    return;
  }

  zone->mapZone->adminState = ADMIN_STATE_CLOSING;
  flushDirtyLists(zone->dirtyLists);
  checkForIOComplete(zone);
}

/**********************************************************************/
void suspendZoneTrees(BlockMapTreeZone *zone, VDOCompletion *completion)
{
  suspendObjectPool(zone->vioPool, completion);
}

/**********************************************************************/
void resumeZoneTrees(BlockMapTreeZone *zone)
{
  resumeObjectPool(zone->vioPool);
}

/**
 * Release a lock on a page which was being loaded or allocated.
 *
 * @param dataVIO  The DataVIO releasing the page lock
 * @param what     What the DataVIO was doing (for logging)
 **/
static void releasePageLock(DataVIO *dataVIO, char *what)
{
  TreeLock *lock = &dataVIO->treeLock;
  ASSERT_LOG_ONLY(lock->locked,
                  "release of unlocked block map page %s for key %" PRIu64
                  " in tree %u",
                  what, lock->key, lock->rootIndex);
  BlockMapTreeZone *zone       = getBlockMapTreeZone(dataVIO);
  TreeLock         *lockHolder = intMapRemove(zone->loadingPages, lock->key);
  ASSERT_LOG_ONLY((lockHolder == lock),
                  "block map page %s mismatch for key %" PRIu64 " in tree %u",
                  what, lock->key, lock->rootIndex);
  lock->locked = false;
}

/**
 * Continue a DataVIO now that the lookup is complete.
 *
 * @param dataVIO  The DataVIO
 * @param result   The result of the lookup
 **/
static void finishLookup(DataVIO *dataVIO, int result)
{
  dataVIO->treeLock.height = 0;

  BlockMapTreeZone *zone       = getBlockMapTreeZone(dataVIO);
  VDOCompletion    *completion = dataVIOAsCompletion(dataVIO);
  setCompletionResult(completion, result);
  launchCallback(completion, dataVIO->treeLock.callback,
                 dataVIO->treeLock.threadID);
  if ((--zone->activeLookups == 0)
      && (zone->mapZone->adminState == ADMIN_STATE_CLOSE_REQUESTED)) {
    closeZoneTrees(zone);
  }
}

/**
 * Abort a block map PBN lookup due to an error in the load or allocation on
 * which we were waiting.
 *
 * @param waiter   The DataVIO which was waiting for a page load or allocation
 * @param context  The error which caused the abort
 **/
static void abortLookupForWaiter(Waiter *waiter, void *context)
{
  DataVIO *dataVIO = waiterAsDataVIO(waiter);
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
 * @param dataVIO  The DataVIO which was loading or allocating a page
 * @param result   The error code
 * @param what     What the DataVIO was doing (for logging)
 **/
static void abortLookup(DataVIO *dataVIO, int result, char *what)
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
 * @param dataVIO  The DataVIO doing the page load
 * @param result   The error code
 **/
static void abortLoad(DataVIO *dataVIO, int result)
{
  abortLookup(dataVIO, result, "load");
}

/**
 * Determine if a location represents a valid mapping for a tree page.
 *
 * @param vdo      The VDO
 * @param mapping  The DataLocation to check
 * @param height   The height of the entry in the tree
 *
 * @return <code>true</code> if the entry represents a invalid page mapping
 **/
__attribute__((warn_unused_result))
static bool isInvalidTreeEntry(const VDO          *vdo,
                               const DataLocation *mapping,
                               Height              height)
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
static void loadBlockMapPage(BlockMapTreeZone *zone, DataVIO *dataVIO);
static void allocateBlockMapPage(BlockMapTreeZone *zone, DataVIO *dataVIO);

/**
 * Continue a block map PBN lookup now that a page has been loaded by
 * descending one level in the tree.
 *
 * @param dataVIO  The DataVIO doing the lookup
 * @param page     The page which was just loaded
 **/
static void continueWithLoadedPage(DataVIO *dataVIO, BlockMapPage *page)
{
  TreeLock         *lock = &dataVIO->treeLock;
  BlockMapTreeSlot  slot = lock->treeSlots[lock->height];
  DataLocation mapping
    = unpackBlockMapEntry(&page->entries[slot.blockMapSlot.slot]);
  if (isInvalidTreeEntry(getVDOFromDataVIO(dataVIO), &mapping, lock->height)) {
    logErrorWithStringError(VDO_BAD_MAPPING,
                            "Invalid block map tree PBN: %" PRIu64 " with "
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
 * @param waiter   The DataVIO waiting for a page to be loaded
 * @param context  The page which was just loaded
 **/
static void continueLoadForWaiter(Waiter *waiter, void *context)
{
  DataVIO *dataVIO = waiterAsDataVIO(waiter);
  dataVIO->treeLock.height--;
  continueWithLoadedPage(dataVIO, (BlockMapPage *) context);
}

/**
 * Finish loading a page now that it has been read in from disk. This callback
 * is registered in loadPage().
 *
 * @param completion  The VIO doing the page read
 **/
static void finishBlockMapPageLoad(VDOCompletion *completion)
{
  VIOPoolEntry     *entry    = completion->parent;
  DataVIO          *dataVIO  = entry->parent;
  BlockMapTreeZone *zone     = (BlockMapTreeZone *) entry->context;
  BlockMapTree     *tree     = getTree(zone, dataVIO);
  TreeLock         *treeLock = &dataVIO->treeLock;

  treeLock->height--;
  PhysicalBlockNumber pbn
    = treeLock->treeSlots[treeLock->height].blockMapSlot.pbn;
  TreePage     *treePage = getTreePage(zone, tree, treeLock);
  BlockMapPage *page     = (BlockMapPage *) treePage->pageBuffer;
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
 * @param completion  The VIO doing the page read
 **/
static void handleIOError(VDOCompletion *completion)
{
  int               result  = completion->result;
  VIOPoolEntry     *entry   = completion->parent;
  DataVIO          *dataVIO = entry->parent;
  BlockMapTreeZone *zone    = (BlockMapTreeZone *) entry->context;
  returnVIOToPool(zone->vioPool, entry);
  abortLoad(dataVIO, result);
}

/**
 * Read a tree page from disk now that we've gotten a VIO with which to do the
 * read. This WaiterCallback is registered in loadBlockMapPage().
 *
 * @param waiter   The DataVIO which requires a page load
 * @param context  The VIOPool entry with which to do the read
 **/
static void loadPage(Waiter *waiter, void *context)
{
  VIOPoolEntry *entry   = context;
  DataVIO      *dataVIO = waiterAsDataVIO(waiter);

  entry->parent = dataVIO;
  entry->vio->completion.callbackThreadID
    = getBlockMapForZone(dataVIO->logical.zone)->threadID;

  TreeLock *lock = &dataVIO->treeLock;
  launchReadMetadataVIO(entry->vio,
                        lock->treeSlots[lock->height - 1].blockMapSlot.pbn,
                        finishBlockMapPageLoad, handleIOError);
}

/**
 * Attempt to acquire a lock on a page in the block map tree. If the page is
 * already locked, queue up to wait for the lock to be released. If the lock is
 * acquired, the DataVIO's treeLock.locked field will be set to true.
 *
 * @param zone     The BlockMapTreeZone in which the DataVIO operates
 * @param dataVIO  The DataVIO which desires a page lock
 *
 * @return VDO_SUCCESS or an error
 **/
static int attemptPageLock(BlockMapTreeZone *zone, DataVIO *dataVIO)
{
  TreeLock         *lock     = &dataVIO->treeLock;
  Height            height   = lock->height;
  BlockMapTreeSlot  treeSlot = lock->treeSlots[height];
  PageKey           key;
  key.descriptor = (PageDescriptor) {
    .rootIndex = lock->rootIndex,
    .height    = height,
    .pageIndex = treeSlot.pageIndex,
    .slot      = treeSlot.blockMapSlot.slot,
  };
  lock->key = key.key;

  TreeLock *lockHolder;
  int result = intMapPut(zone->loadingPages, lock->key, lock, false,
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
 * @param zone     The BlockMapTreeZone in which the DataVIO operates
 * @param dataVIO  The DataVIO which requires a page to be loaded
 **/
static void loadBlockMapPage(BlockMapTreeZone *zone, DataVIO *dataVIO)
{
  int result = attemptPageLock(zone, dataVIO);
  if (result != VDO_SUCCESS) {
    abortLoad(dataVIO, result);
    return;
  }

  if (dataVIO->treeLock.locked) {
    Waiter *waiter   = dataVIOAsWaiter(dataVIO);
    waiter->callback = loadPage;
    result = acquireVIOFromPool(zone->vioPool, waiter);
    if (result != VDO_SUCCESS) {
      abortLoad(dataVIO, result);
    }
  }
}

/**
 * Set the callback of a DataVIO after it has allocated a block map page.
 *
 * @param dataVIO  The DataVIO
 **/
static void setPostAllocationCallback(DataVIO *dataVIO)
{
  setCallback(dataVIOAsCompletion(dataVIO), dataVIO->treeLock.callback,
              dataVIO->treeLock.threadID);
}

/**
 * Abort a block map PBN lookup due to an error allocating a page.
 *
 * @param dataVIO  The DataVIO doing the page allocation
 * @param result   The error code
 **/
static void abortAllocation(DataVIO *dataVIO, int result)
{
  setPostAllocationCallback(dataVIO);
  abortLookup(dataVIO, result, "allocation");
}

/**
 * Callback to handle an error while attempting to allocate a page. This
 * callback is used to transfer back to the logical zone along the block map
 * page allocation path.
 *
 * @param completion  The DataVIO doing the allocation
 **/
static void allocationFailure(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  abortAllocation(dataVIO, completion->result);
}

/**
 * Continue with page allocations now that a parent page has been allocated.
 *
 * @param waiter   The DataVIO which was waiting for a page to be allocated
 * @param context  The physical block number of the page which was just
 *                 allocated
 **/
static void continueAllocationForWaiter(Waiter *waiter, void *context)
{
  DataVIO             *dataVIO  = waiterAsDataVIO(waiter);
  TreeLock            *treeLock = &dataVIO->treeLock;
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
 * @param completion  The DataVIO doing the allocation
 **/
static void finishBlockMapAllocation(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  if (completion->result != VDO_SUCCESS) {
    allocationFailure(completion);
    return;
  }

  BlockMapTreeZone *zone     = getBlockMapTreeZone(dataVIO);
  BlockMapTree     *tree     = getTree(zone, dataVIO);
  TreeLock         *treeLock = &dataVIO->treeLock;
  TreePage         *treePage = getTreePage(zone, tree, treeLock);
  Height            height   = treeLock->height;

  PhysicalBlockNumber pbn = treeLock->treeSlots[height - 1].blockMapSlot.pbn;

  // Record the allocation.
  BlockMapPage   *page    = (BlockMapPage *) treePage->pageBuffer;
  SequenceNumber  oldLock = treePage->recoveryLock;
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
    addToDirtyLists(zone->dirtyLists, &treePage->node, oldLock,
                    treePage->recoveryLock);
  }

  treeLock->height--;
  if (height > 1) {
    // Format the interior node we just allocated (in memory).
    treePage = getTreePage(zone, tree, treeLock);
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
 * @param completion  The DataVIO doing the allocation
 **/
static void releaseBlockMapWriteLock(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  AllocatingVIO *allocatingVIO = dataVIOAsAllocatingVIO(dataVIO);
  assertInAllocatedZone(dataVIO);
  if (completion->result != VDO_SUCCESS) {
    launchLogicalCallback(dataVIO, allocationFailure, THIS_LOCATION(NULL));
    return;
  }

  releaseAllocationLock(allocatingVIO);
  resetAllocation(allocatingVIO);
  launchLogicalCallback(dataVIO, finishBlockMapAllocation,
                        THIS_LOCATION("$F;cb=finishBlockMapAllocation"));
}

/**
 * Set the reference count of a newly allocated block map page to
 * MAXIMUM_REFERENCES now that we have made a recovery journal entry for it.
 * MAXIMUM_REFERENCES is used to prevent deduplication against the block after
 * we release the write lock on it, but before we write out the page.
 *
 * @param completion  The DataVIO doing the allocation
 **/
static void setBlockMapPageReferenceCount(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInAllocatedZone(dataVIO);
  if (completion->result != VDO_SUCCESS) {
    launchLogicalCallback(dataVIO, allocationFailure, THIS_LOCATION(NULL));
    return;
  }

  TreeLock *lock = &dataVIO->treeLock;
  PhysicalBlockNumber pbn = lock->treeSlots[lock->height - 1].blockMapSlot.pbn;
  completion->callback = releaseBlockMapWriteLock;
  addSlabJournalEntry(getSlabJournal(getVDOFromDataVIO(dataVIO)->depot, pbn),
                      dataVIO);
}

/**
 * Make a recovery journal entry for a newly allocated block map page.
 * This callback is registered in continueBlockMapPageAllocation().
 *
 * @param completion  The DataVIO doing the allocation
 **/
static void journalBlockMapAllocation(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
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
 * to allocateDataBlock() by allocateBlockMapPage().
 *
 * @param allocatingVIO  The DataVIO which is doing the allocation
 **/
static void continueBlockMapPageAllocation(AllocatingVIO *allocatingVIO)
{
  DataVIO *dataVIO = allocatingVIOAsDataVIO(allocatingVIO);
  if (!hasAllocation(dataVIO)) {
    setLogicalCallback(dataVIO, allocationFailure, THIS_LOCATION(NULL));
    continueDataVIO(dataVIO, VDO_NO_SPACE);
    return;
  }

  PhysicalBlockNumber  pbn  = allocatingVIO->allocation;
  TreeLock            *lock = &dataVIO->treeLock;
  lock->treeSlots[lock->height - 1].blockMapSlot.pbn = pbn;
  setUpReferenceOperationWithLock(BLOCK_MAP_INCREMENT, pbn,
                                  MAPPING_STATE_UNCOMPRESSED,
                                  allocatingVIO->allocationLock,
                                  &dataVIO->operation);
  launchJournalCallback(dataVIO, journalBlockMapAllocation,
                        THIS_LOCATION("$F;cb=journalBlockMapAllocation"));
}

/**
 * Allocate a block map page.
 *
 * @param zone     The zone in which the DataVIO is operating
 * @param dataVIO  The DataVIO which needs to allocate a page
 **/
static void allocateBlockMapPage(BlockMapTreeZone *zone, DataVIO *dataVIO)
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

  allocateDataBlock(dataVIOAsAllocatingVIO(dataVIO),
                    VIO_BLOCK_MAP_WRITE_LOCK,
                    continueBlockMapPageAllocation);
}

/**********************************************************************/
void lookupBlockMapPBN(DataVIO *dataVIO)
{
  BlockMapTreeZone *zone = getBlockMapTreeZone(dataVIO);
  zone->activeLookups++;
  if (isClosing(zone->mapZone->adminState)) {
    finishLookup(dataVIO, VDO_SHUTTING_DOWN);
    return;
  }


  TreeLock     *lock = &dataVIO->treeLock;
  BlockMapTree *tree = getTree(zone, dataVIO);

  PageNumber pageIndex
    = ((lock->treeSlots[0].pageIndex - zone->mapZone->blockMap->flatPageCount)
       / zone->mapZone->blockMap->rootCount);
  BlockMapTreeSlot treeSlot = {
    .pageIndex = pageIndex / BLOCK_MAP_ENTRIES_PER_PAGE,
    .blockMapSlot = {
      .pbn  = 0,
      .slot = pageIndex % BLOCK_MAP_ENTRIES_PER_PAGE,
    },
  };

  BlockMapPage *page = NULL;
  for (lock->height = 1; lock->height <= BLOCK_MAP_TREE_HEIGHT;
       lock->height++) {
    lock->treeSlots[lock->height] = treeSlot;
    page = (BlockMapPage *) (getTreePage(zone, tree, lock)->pageBuffer);
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
  DataLocation mapping
    = unpackBlockMapEntry(&page->entries[treeSlot.blockMapSlot.slot]);
  if (isInvalidTreeEntry(getVDOFromDataVIO(dataVIO), &mapping, lock->height)) {
    logErrorWithStringError(VDO_BAD_MAPPING,
                            "Invalid block map tree PBN: %" PRIu64 " with "
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
PhysicalBlockNumber findBlockMapPagePBN(BlockMap *map, PageNumber pageNumber)
{
  if (pageNumber < map->flatPageCount) {
    return (BLOCK_MAP_FLAT_PAGE_ORIGIN + pageNumber);
  }

  RootCount  rootIndex = pageNumber % map->rootCount;
  PageNumber pageIndex = ((pageNumber - map->flatPageCount) / map->rootCount);
  SlotNumber slot      = pageIndex % BLOCK_MAP_ENTRIES_PER_PAGE;
  pageIndex /= BLOCK_MAP_ENTRIES_PER_PAGE;

  BlockMapTree *tree     = getTreeFromForest(map->forest, rootIndex);
  TreePage     *treePage = getTreePageByIndex(map->forest, tree, 1, pageIndex);
  BlockMapPage *page     = (BlockMapPage *) treePage->pageBuffer;
  if (!isBlockMapPageInitialized(page)) {
    return ZERO_BLOCK;
  }

  DataLocation mapping = unpackBlockMapEntry(&page->entries[slot]);
  if (!isValidLocation(&mapping) || isCompressed(mapping.state)) {
    return ZERO_BLOCK;
  }
  return mapping.pbn;
}

/**********************************************************************/
void writeTreePage(TreePage *page, BlockMapTreeZone *zone)
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
