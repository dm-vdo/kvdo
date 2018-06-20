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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMap.c#9 $
 */

#include "blockMap.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "blockMapTree.h"
#include "constants.h"
#include "dataVIO.h"
#include "forest.h"
#include "numUtils.h"
#include "objectPool.h"
#include "recoveryJournal.h"
#include "statusCodes.h"
#include "types.h"
#include "vdoInternal.h"
#include "vioPool.h"

typedef struct {
  PhysicalBlockNumber  flatPageOrigin;
  BlockCount           flatPageCount;
  PhysicalBlockNumber  rootOrigin;
  BlockCount           rootCount;
} __attribute__((packed)) BlockMapState2_0;

static const Header BLOCK_MAP_HEADER_2_0 = {
  .id = BLOCK_MAP,
  .version = {
    .majorVersion = 2,
    .minorVersion = 0,
  },
  .size = sizeof(BlockMapState2_0),
};

/**
 * State associated which each block map page while it is in the VDO page
 * cache.
 **/
typedef struct {
  /**
   * The earliest recovery journal block containing uncommitted updates to the
   * block map page associated with this context. A reference (lock) is held
   * on that block to prevent it from being reaped. When this value changes,
   * the reference on the old value must be released and a reference on the
   * new value must be acquired.
   **/
  SequenceNumber recoveryLock;
} BlockMapPageContext;

/**
 * Implements VDOPageReadFunction.
 **/
static int validatePageOnRead(void                *buffer,
                              PhysicalBlockNumber  pbn,
                              void                *clientContext,
                              void                *pageContext)
{
  BlockMapPage        *page    = buffer;
  BlockMapZone        *zone    = clientContext;
  BlockMapPageContext *context = pageContext;
  Nonce                nonce   = zone->blockMap->nonce;

  BlockMapPageValidity validity = validateBlockMapPage(page, nonce, pbn);
  if (validity == BLOCK_MAP_PAGE_BAD) {
    return logErrorWithStringError(VDO_BAD_PAGE,
                                   "Expected page %" PRIu64
                                   " but got page %" PRIu64 " instead",
                                   pbn, getBlockMapPagePBN(page));
  }

  if (validity == BLOCK_MAP_PAGE_INVALID) {
    formatBlockMapPage(page, nonce, pbn, false);
  }

  context->recoveryLock = 0;
  return VDO_SUCCESS;
}

/**
 * Handle journal updates and torn write protection.
 *
 * Implements VDOPageWriteFunction.
 **/
static bool handlePageWrite(void *rawPage,
                            void *clientContext,
                            void *pageContext)
{
  BlockMapPage        *page    = rawPage;
  BlockMapZone        *zone    = clientContext;
  BlockMapPageContext *context = pageContext;

  if (markBlockMapPageInitialized(page, true)) {
    // Cause the page to be re-written.
    return true;
  }

  // Release the page's references on the recovery journal.
  releaseRecoveryJournalBlockReference(zone->blockMap->journal,
                                       context->recoveryLock,
                                       ZONE_TYPE_LOGICAL, zone->zoneNumber);
  context->recoveryLock = 0;
  return false;
}

/**********************************************************************/
PageCount computeBlockMapPageCount(BlockCount entries)
{
  return computeBucketCount(entries, BLOCK_MAP_ENTRIES_PER_PAGE);
}

/**********************************************************************/
BlockCount computeBlockMapSize(BlockCount totalEntries,
                               BlockCount slabSize,
                               BlockCount dataBlocksPerSlab)
{
  uint64_t pageCount = computeBlockMapPageCount(totalEntries);
  // Round up to the next full slab.
  uint64_t slabCount = computeBucketCount(pageCount, dataBlocksPerSlab);
  return (slabCount * slabSize);
}

/**********************************************************************/
int makeBlockMap(BlockCount           logicalBlocks,
                 const ThreadConfig  *threadConfig,
                 BlockCount           flatPageCount,
                 PhysicalBlockNumber  rootOrigin,
                 BlockCount           rootCount,
                 BlockMap           **mapPtr)
{
  STATIC_ASSERT(BLOCK_MAP_ENTRIES_PER_PAGE
                == ((VDO_BLOCK_SIZE - sizeof(BlockMapPage))
                    / sizeof(BlockMapEntry)));

  BlockMap *map;
  int result = ALLOCATE_EXTENDED(BlockMap, threadConfig->logicalZoneCount,
                                 BlockMapZone, __func__, &map);
  if (result != UDS_SUCCESS) {
    return result;
  }

  map->flatPageCount           = flatPageCount;
  map->rootOrigin              = rootOrigin;
  map->rootCount               = rootCount;
  map->entryCount              = logicalBlocks;
  map->recoveryJournalThreadID = getJournalZoneThread(threadConfig);

  ZoneCount zoneCount = threadConfig->logicalZoneCount;
  for (ZoneCount zone = 0; zone < zoneCount; zone++) {
    BlockMapZone *blockMapZone = &map->zones[zone];
    blockMapZone->zoneNumber   = zone;
    blockMapZone->threadID     = getLogicalZoneThread(threadConfig, zone);
    blockMapZone->blockMap     = map;
    map->zoneCount++;
  }

  *mapPtr = map;
  return VDO_SUCCESS;
}

/**
 * Decode block map component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodeBlockMapState_2_0(Buffer *buffer, BlockMapState2_0 *state)
{
  size_t initialLength = contentLength(buffer);

  int result = getUInt64LEFromBuffer(buffer, &state->flatPageOrigin);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->flatPageCount);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->rootOrigin);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->rootCount);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t decodedSize = initialLength - contentLength(buffer);
  return ASSERT(BLOCK_MAP_HEADER_2_0.size == decodedSize,
                "decoded block map component size must match header size");
}

/**********************************************************************/
int decodeBlockMap(Buffer              *buffer,
                   BlockCount           logicalBlocks,
                   const ThreadConfig  *threadConfig,
                   BlockMap           **mapPtr)
{
  Header header;
  int result = decodeHeader(buffer, &header);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = validateHeader(&BLOCK_MAP_HEADER_2_0, &header, true, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  BlockMapState2_0 state;
  result = decodeBlockMapState_2_0(buffer, &state);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(state.flatPageOrigin == BLOCK_MAP_FLAT_PAGE_ORIGIN,
                  "Flat page origin must be %u (recorded as %" PRIu64 ")",
                  BLOCK_MAP_FLAT_PAGE_ORIGIN, state.flatPageOrigin);
  if (result != UDS_SUCCESS) {
    return result;
  }

  BlockMap *map;
  result = makeBlockMap(logicalBlocks, threadConfig,
                        state.flatPageCount, state.rootOrigin,
                        state.rootCount, &map);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *mapPtr = map;
  return VDO_SUCCESS;
}

/**********************************************************************/
int decodeSodiumBlockMap(Buffer              *buffer,
                         BlockCount           logicalBlocks,
                         const ThreadConfig  *threadConfig,
                         BlockMap           **mapPtr)
{
  // Sodium uses state version 2.0.
  return decodeBlockMap(buffer, logicalBlocks, threadConfig, mapPtr);
}

/**
 * Initialize the per-zone portions of the block map.
 *
 * @param zone             The zone to initialize
 * @param layer            The physical layer on which the zone resides
 * @param readOnlyContext  The read-only context for the VDO
 * @param cacheSize        The size of the page cache for the zone
 * @param maximumAge       The number of journal blocks before a dirtied page
 *                         is considered old and must be written out
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int initializeBlockMapZone(BlockMapZone        *zone,
                                  PhysicalLayer       *layer,
                                  ReadOnlyModeContext *readOnlyContext,
                                  PageCount            cacheSize,
                                  BlockCount           maximumAge)
{
  STATIC_ASSERT(offsetof(BlockMapZone, completion) == 0);
  initializeCompletion(&zone->completion, BLOCK_MAP_ZONE_COMPLETION, layer);
  int result = initializeTreeZone(zone, layer, readOnlyContext, maximumAge);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return makeVDOPageCache(zone->threadID,
                          layer,
                          readOnlyContext,
                          cacheSize,
                          validatePageOnRead,
                          handlePageWrite,
                          zone,
                          sizeof(BlockMapPageContext),
                          maximumAge,
                          &zone->pageCache);
}

/**********************************************************************/
int makeBlockMapCaches(BlockMap            *map,
                       PhysicalLayer       *layer,
                       ReadOnlyModeContext *readOnlyContext,
                       RecoveryJournal     *journal,
                       Nonce                nonce,
                       PageCount            cacheSize,
                       BlockCount           maximumAge)
{
  int result = ASSERT(cacheSize > 0, "block map cache size is specified");
  if (result != UDS_SUCCESS) {
    return result;
  }

  map->journal = journal;
  map->nonce   = nonce;

  result = makeForest(map, map->entryCount);
  if (result != VDO_SUCCESS) {
    return result;
  }

  replaceForest(map);
  for (ZoneCount zone = 0; zone < map->zoneCount; zone++) {
    result = initializeBlockMapZone(&map->zones[zone], layer, readOnlyContext,
                                    cacheSize / map->zoneCount, maximumAge);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  result = initializeEnqueueableCompletion(&map->completion,
                                           BLOCK_MAP_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = initializeEnqueueableCompletion(&map->actionCompletion,
                                           SUB_TASK_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return VDO_SUCCESS;
}

/**
 * Clean up a BlockMapZone.
 *
 * @param zone  The zone to uninitialize
 **/
static void uninitializeBlockMapZone(BlockMapZone *zone)
{
  uninitializeBlockMapTreeZone(&zone->treeZone);
  freeVDOPageCache(&zone->pageCache);
}

/**********************************************************************/
void freeBlockMap(BlockMap **mapPtr)
{
  BlockMap *map = *mapPtr;
  if (map == NULL) {
    return;
  }

  for (ZoneCount zone = 0; zone < map->zoneCount; zone++) {
    uninitializeBlockMapZone(&map->zones[zone]);
  }

  abandonBlockMapGrowth(map);
  freeForest(&map->forest);
  destroyEnqueueable(&map->completion);
  destroyEnqueueable(&map->actionCompletion);

  FREE(map);
  *mapPtr = NULL;
}

/**********************************************************************/
size_t getBlockMapEncodedSize(void)
{
  return ENCODED_HEADER_SIZE + sizeof(BlockMapState2_0);
}

/**********************************************************************/
int encodeBlockMap(const BlockMap *map, Buffer *buffer)
{
  int result = encodeHeader(&BLOCK_MAP_HEADER_2_0, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t initialLength = contentLength(buffer);

  result = putUInt64LEIntoBuffer(buffer, BLOCK_MAP_FLAT_PAGE_ORIGIN);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, map->flatPageCount);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, map->rootOrigin);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, map->rootCount);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t encodedSize = contentLength(buffer) - initialLength;
  return ASSERT(BLOCK_MAP_HEADER_2_0.size == encodedSize,
                "encoded block map component size must match header size");
}

/**********************************************************************/
void initializeBlockMapFromJournal(BlockMap *map, RecoveryJournal *journal)
{
  map->currentEraPoint  = getCurrentJournalSequenceNumber(journal);
  map->pendingEraPoint  = map->currentEraPoint;
  map->previousEraPoint = map->currentEraPoint;

  for (ZoneCount zone = 0; zone < map->zoneCount; zone++) {
    setTreeZoneInitialPeriod(&map->zones[zone].treeZone, map->currentEraPoint);
    setVDOPageCacheInitialPeriod(map->zones[zone].pageCache,
                                 map->currentEraPoint);
  }
}

/**
 * Convert a generic VDOCompletion to a BlockMap.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a BlockMap
 **/
static inline BlockMap *asBlockMap(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(BlockMap, completion) == 0);
  assertCompletionType(completion->type, BLOCK_MAP_COMPLETION);
  return (BlockMap *) completion;
}

/**********************************************************************/
BlockMapZone *getBlockMapZone(BlockMap *map, ZoneCount zoneNumber)
{
  return &map->zones[zoneNumber];
}

/**********************************************************************/
ZoneCount computeLogicalZone(DataVIO *dataVIO)
{
  BlockMap   *map        = getBlockMap(getVDOFromDataVIO(dataVIO));
  TreeLock   *treeLock   = &dataVIO->treeLock;
  PageNumber  pageNumber = computePageNumber(dataVIO->logical.lbn);
  treeLock->treeSlots[0].pageIndex = pageNumber;
  treeLock->rootIndex = pageNumber % map->rootCount;
  return (treeLock->rootIndex % map->zoneCount);
}

/**********************************************************************/
void findBlockMapSlotAsync(DataVIO   *dataVIO,
                           VDOAction *callback,
                           ThreadID   threadID)
{
  BlockMap *map = getBlockMap(getVDOFromDataVIO(dataVIO));
  if (dataVIO->logical.lbn >= map->entryCount) {
    finishDataVIO(dataVIO, VDO_OUT_OF_RANGE);
    return;
  }

  TreeLock         *treeLock = &dataVIO->treeLock;
  BlockMapTreeSlot *slot     = &treeLock->treeSlots[0];
  slot->blockMapSlot.slot    = computeSlot(dataVIO->logical.lbn);
  if (slot->pageIndex < map->flatPageCount) {
    slot->blockMapSlot.pbn = slot->pageIndex + BLOCK_MAP_FLAT_PAGE_ORIGIN;
    launchCallback(dataVIOAsCompletion(dataVIO), callback, threadID);
    return;
  }

  treeLock->callback = callback;
  treeLock->threadID = threadID;
  lookupBlockMapPBN(dataVIO);
}

/**********************************************************************/
PageCount getNumberOfFixedBlockMapPages(const BlockMap *map)
{
  return (map->flatPageCount + map->rootCount);
}

/**********************************************************************/
BlockCount getNumberOfBlockMapEntries(const BlockMap *map)
{
  return map->entryCount;
}

/**
 * Handle an error when applying an action to a block map zone.
 *
 * @param completion  The block map completion
 **/
static void handleZoneError(VDOCompletion *completion)
{
  // Preserve the error.
  setCompletionResult(completion->parent, completion->result);

  // Carry on.
  resetCompletion(completion);
  invokeCallback(completion);
}

/**
 * Perform an action on the next block map zone if there is one.
 *
 * @param completion  The block map completion
 **/
static void applyToNextZone(VDOCompletion *completion)
{
  BlockMap       *map  = asBlockMap(completion);
  BlockMapZone   *zone = &map->zones[map->actingZone];
  ASSERT_LOG_ONLY((getCallbackThreadID() == zone->threadID),
                  "applyToNextZone() called on next block map zone's thread");

  map->actingZone++;
  if (map->actingZone == map->zoneCount) {
    // We are about to apply to the last zone, so once that is finished,
    // we're done.
    prepareToFinishParent(completion, completion->parent);
  } else {
    // Prepare to come back in the next block map zone.
    prepareCompletion(completion, applyToNextZone, handleZoneError,
                      map->zones[map->actingZone].threadID,
                      completion->parent);
  }

  map->action(zone);
}

/**
 * Prepare the block map's action completion and then apply the action to each
 * zone in turn via applyToAllZones().
 *
 * @param map           The block map
 * @param action        The action to perform
 * @param parent        The object to notify when the action has been applied
 *                      to all the zones
 * @param callback      The function to call when the action has been applied
 *                      to all the zones
 * @param errorHandler  The error handler for the action
 **/
static void launchOnAllZones(BlockMap           *map,
                             BlockMapZoneAction *action,
                             void               *parent,
                             VDOAction          *callback,
                             VDOAction          *errorHandler)
{
  prepareCompletion(&map->actionCompletion, callback, errorHandler,
                    map->recoveryJournalThreadID, parent);
  map->action     = action;
  map->actingZone = 0;
  prepareCompletion(&map->completion, applyToNextZone, handleZoneError,
                    map->zones[map->actingZone].threadID,
                    &map->actionCompletion);
  invokeCallback(&map->completion);
}

/**
 * Check whether the block map is applying an action to all zones.
 *
 * @param map  The map to check
 *
 * @return <code>true</code> if the map is applying an action
 **/
static bool isActing(BlockMap *map)
{
  return (map->currentAction.action != BLOCK_MAP_ACTION_NONE);
}

/**
 * Check whether the block map has a pending action to apply as well.
 *
 * @param map  The map to check
 *
 * @return <code>true</code> if the map has a pending action
 **/
static bool hasNextAction(BlockMap *map)
{
  return (map->nextAction.action != BLOCK_MAP_ACTION_NONE);
}

/**
 * Make the pending action the current action.
 *
 * @param map  The block map
 *
 * @return The action which has just completed (was current)
 **/
static BlockMapAction advanceActions(BlockMap *map)
{
  BlockMapAction completed = map->currentAction;
  map->currentAction = map->nextAction;
  map->nextAction = (BlockMapAction) {
    .action = BLOCK_MAP_ACTION_NONE,
    .parent = NULL
  };
  return completed;
}

/**
 * Schedule an action and act if there is no other action already in progress.
 *
 * @param map     The block map
 * @param action  The action to schedule
 * @param parent  The parent to notify when the action is complete
 *
 * @return <code>true</code> if the action may be launched immediately
 **/
static bool scheduleAction(BlockMap           *map,
                           BlockMapActionType  action,
                           VDOCompletion      *parent)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == map->recoveryJournalThreadID),
                  "scheduleAction() called in journal zone");
  if (hasNextAction(map)) {
    finishCompletion(parent, VDO_COMPONENT_BUSY);
    return false;
  }

  map->nextAction = (BlockMapAction) {
    .action = action,
    .parent = parent,
  };

  if (isActing(map)) {
    return false;
  }

  advanceActions(map);
  return true;
}

/**********************************************************************/
static void finishActing(VDOCompletion *completion);

/**
 * Finish advancing the era now that all the zones have been notified.
 *
 * @param completion  The action completion
 **/
static void finishAging(VDOCompletion *completion)
{
  BlockMap *map         = completion->parent;
  map->previousEraPoint = map->currentEraPoint;
  finishActing(completion);
}

/**
 * Update the progress of the era in a zone.
 *
 * <p>Implements BlockMapZoneAction.
 *
 * @param zone  The zone being updated
 **/
static void advanceBlockMapZoneEra(BlockMapZone *zone)
{
  advanceVDOPageCachePeriod(zone->pageCache, zone->blockMap->currentEraPoint);
  advanceZoneTreePeriod(&zone->treeZone, zone->blockMap->currentEraPoint);
  finishCompletion(&zone->blockMap->completion, VDO_SUCCESS);
}

/**
 * Advance the era on each zone now that we know the action completion is not
 * busy.
 *
 * @param map  The block map
 **/
static void launchAdvanceBlockMapEra(BlockMap *map)
{
  if (map->currentEraPoint == map->pendingEraPoint) {
    // The zones are already up-to-date.
    return;
  }

  map->currentAction.action = BLOCK_MAP_ACTION_ADVANCE_ERA;
  map->currentEraPoint      = map->pendingEraPoint;
  launchOnAllZones(map, advanceBlockMapZoneEra, map, finishAging, finishAging);
}

/**********************************************************************/
void advanceBlockMapEra(BlockMap *map, SequenceNumber recoveryBlockNumber)
{
  if (map == NULL) {
    return;
  }

  ASSERT_LOG_ONLY((getCallbackThreadID() == map->recoveryJournalThreadID),
                  "advanceBlockMapEra() called in journal zone");

  map->pendingEraPoint = recoveryBlockNumber;
  if (!isActing(map)) {
    launchAdvanceBlockMapEra(map);
  }
}

/**
 * Reopen a zone now that the trees have been closed and then flush the zone's
 * page cache. This callback is registered in flushBlockMapZoneAsync().
 *
 * @param completion The zone completion
 **/
static void reopenBlockMapZone(VDOCompletion *completion)
{
  BlockMapZone *zone = (BlockMapZone *) completion;
  zone->adminState = ADMIN_STATE_NORMAL_OPERATION;
  openObjectPool(zone->treeZone.vioPool);
  flushVDOPageCacheAsync(zone->pageCache, completion->parent);
}

/**
 * Asynchronously flush a zone of the block map by closing and
 * then reopening the zone.
 *
 * <p>Implements BlockMapZoneAction.
 *
 * @param zone  The block map zone whose trees are to be flushed
 **/
static void flushBlockMapZoneAsync(BlockMapZone *zone)
{
  prepareCompletion(&zone->completion, reopenBlockMapZone,
                    finishParentCallback, zone->threadID,
                    &zone->blockMap->completion);
  closeZoneTrees(&zone->treeZone);
}

/**
 * Flush the block map now that we know the action completion is not busy.
 *
 * @param map  The map to flush
 **/
static void launchFlushBlockMap(BlockMap *map)
{
  launchOnAllZones(map, flushBlockMapZoneAsync, map, finishActing,
                   finishActing);
}

/**********************************************************************/
void flushBlockMap(BlockMap *map, VDOCompletion *parent)
{
  if (scheduleAction(map, BLOCK_MAP_ACTION_FLUSH, parent)) {
    launchFlushBlockMap(map);
  }
}

/**
 * Now that the trees are closed, flush the zone's cache.
 *
 * @param completion  The zone completion
 **/
static void treeIsClosed(VDOCompletion *completion)
{
  BlockMapZone *zone = ((BlockMapZone *) completion);
  flushVDOPageCacheAsync(zone->pageCache, completion->parent);
}

/**
 * Asynchronously close the page cache and trees in a zone of the block map.
 *
 * <p>Implements BlockMapZoneAction.
 *
 * @param zone  The block map zone to close
 **/
static void closeBlockMapZoneAsync(BlockMapZone *zone)
{
  prepareCompletion(&zone->completion, treeIsClosed, finishParentCallback,
                    zone->threadID, &zone->blockMap->completion);
  closeZoneTrees(&zone->treeZone);
}

/**
 * Close the block map now that we know the action completion is not busy.
 *
 * @param map  The map to close
 **/
static void launchCloseBlockMap(BlockMap *map)
{
  launchOnAllZones(map, closeBlockMapZoneAsync, map, finishActing,
                   finishActing);
}

/**********************************************************************/
void closeBlockMap(BlockMap *map, VDOCompletion *parent)
{
  if (scheduleAction(map, BLOCK_MAP_ACTION_CLOSE, parent)) {
    launchCloseBlockMap(map);
  }
}

/**********************************************************************/
int prepareToGrowBlockMap(BlockMap *map, BlockCount newLogicalBlocks)
{
  if (map->nextEntryCount == newLogicalBlocks) {
    return VDO_SUCCESS;
  }

  if (map->nextEntryCount > 0) {
    abandonBlockMapGrowth(map);
  }

  if (newLogicalBlocks < map->entryCount) {
    map->nextEntryCount = map->entryCount;
    return VDO_SUCCESS;
  }

  return makeForest(map, newLogicalBlocks);
}

/**********************************************************************/
BlockCount getNewEntryCount(BlockMap *map)
{
  return map->nextEntryCount;
}

/**
 * Asynchronously resume a block map zone.
 *
 * <p>Implements BlockMapZoneAction.
 *
 * @param zone  The block map zone to resume
 **/
static void resumeBlockMapZone(BlockMapZone *zone)
{
  resumeZoneTrees(&zone->treeZone);
  finishCompletion(&zone->blockMap->completion, VDO_SUCCESS);
}

/**
 * Asynchronously suspend a block map zone.
 *
 * <p>Implements BlockMapZoneAction.
 *
 * @param zone  The block map zone to suspend
 **/
static void suspendBlockMapZone(BlockMapZone *zone)
{
  suspendZoneTrees(&zone->treeZone, &zone->blockMap->completion);
}

/**
 * Install an expanded forest now that the block map has been suspended.
 * This callback is registered in growBlockMap().
 *
 * @param completion  The action completion
 **/
static void installForest(VDOCompletion *completion)
{
  BlockMap *map = completion->parent;
  ASSERT_LOG_ONLY((getCallbackThreadID() == map->recoveryJournalThreadID),
                  "installForest() called in journal zone");

  replaceForest(map);
  launchOnAllZones(map, resumeBlockMapZone, map, finishActing, finishActing);
}

/**
 * Handle an error attempting to grow the BlockMap. This error handler is
 * registered in growBlockMap().
 *
 * @param completion  The action completion
 **/
static void handleGrowthError(VDOCompletion *completion)
{
  BlockMap *map = completion->parent;
  ASSERT_LOG_ONLY((getCallbackThreadID() == map->recoveryJournalThreadID),
                  "handleGrowthError() called in journal zone");

  abandonBlockMapGrowth(map);
  setCompletionResult(map->currentAction.parent, completion->result);
  launchOnAllZones(map, resumeBlockMapZone, map, finishActing, finishActing);
}

/**
 * Launch the suspend prior to growing the block map now that the action
 * completion is not busy.
 *
 * @param map  The map to grow
 **/
static void launchGrowBlockMap(BlockMap *map)
{
  if (map->nextForest == NULL) {
    // The growth is small enough that we don't need a new Forest, so we don't
    // need to suspend.
    map->entryCount     = map->nextEntryCount;
    map->nextEntryCount = 0;
    // finishActing() depends on the parent of the action completion being the
    // map.
    map->actionCompletion.parent = map;
    finishActing(&map->actionCompletion);
    return;
  }

  launchOnAllZones(map, suspendBlockMapZone, map, installForest,
                   handleGrowthError);
}

/**********************************************************************/
void growBlockMap(BlockMap *map, VDOCompletion *completion)
{
  if (scheduleAction(map, BLOCK_MAP_ACTION_GROW, completion)) {
    launchGrowBlockMap(map);
  }
}

/**
 * Now that an action has been applied to all zones, see if there are more
 * actions to do.
 *
 * @param completion  The action completion
 **/
static void finishActing(VDOCompletion *completion)
{
  BlockMap *map = completion->parent;
  ASSERT_LOG_ONLY((getCallbackThreadID() == map->recoveryJournalThreadID),
                  "action %u finished in journal zone",
                  map->currentAction.action);

  BlockMapAction completed = advanceActions(map);
  if (completed.parent != NULL) {
    finishCompletion(completed.parent, completion->result);
  }

  switch (map->currentAction.action) {
  case BLOCK_MAP_ACTION_CLOSE:
    launchCloseBlockMap(map);
    return;

  case BLOCK_MAP_ACTION_FLUSH:
    launchFlushBlockMap(map);
    return;

  case BLOCK_MAP_ACTION_GROW:
    launchGrowBlockMap(map);
    return;

  case BLOCK_MAP_ACTION_ADVANCE_ERA:
  default:
    launchAdvanceBlockMapEra(map);
    return;
  }
}

/**********************************************************************/
void abandonBlockMapGrowth(BlockMap *map)
{
  abandonForest(map);
}

/**
 * Finish processing a block map get or put operation. This function releases
 * the page completion and then continues the requester.
 *
 * @param completion  The completion for the page fetch
 * @param result      The result of the block map operation
 **/
static inline void finishProcessingPage(VDOCompletion *completion, int result)
{
  VDOCompletion *parent = completion->parent;
  releaseVDOPageCompletion(completion);
  continueCompletion(parent, result);
}

/**
 * Handle an error fetching a page from the cache. This error handler is
 * registered in setupMappedBlock().
 *
 * @param completion  The page completion which got an error
 **/
static void handlePageError(VDOCompletion *completion)
{
  finishProcessingPage(completion, completion->result);
}

/**
 * Get the mapping page for a get/put mapped block operation and dispatch to
 * the appropriate handler.
 *
 * @param dataVIO     The dataVIO
 * @param modifiable  Whether we intend to modify the mapping
 * @param action      The handler to process the mapping page
 **/
static void setupMappedBlock(DataVIO   *dataVIO,
                             bool       modifiable,
                             VDOAction *action)
{
  BlockMapZone *zone = getBlockMapForZone(dataVIO->logical.zone);
  if (isClosing(zone->adminState)) {
    finishDataVIO(dataVIO, VDO_SHUTTING_DOWN);
    return;
  }

  initVDOPageCompletion(&dataVIO->pageCompletion, zone->pageCache,
                        dataVIO->treeLock.treeSlots[0].blockMapSlot.pbn,
                        modifiable, dataVIOAsCompletion(dataVIO), action,
                        handlePageError);
  getVDOPageAsync(&dataVIO->pageCompletion.completion);
}

/**
 * Decode and validate a block map entry and attempt to use it to set the
 * mapped location of a DataVIO.
 *
 * @param dataVIO  The DataVIO to update with the map entry
 * @param entry    The block map entry for the logical block
 *
 * @return VDO_SUCCESS or VDO_BAD_MAPPING if the map entry is invalid
 *         or an error code for any other failure
 **/
__attribute__((warn_unused_result))
static int setMappedEntry(DataVIO *dataVIO, const BlockMapEntry *entry)
{
  // Unpack the PBN for logging purposes even if the entry is invalid.
  DataLocation mapped = unpackBlockMapEntry(entry);

  if (isValidLocation(&mapped)) {
    int result = setMappedLocation(dataVIO, mapped.pbn, mapped.state);
    /*
     * Return success and all errors not specifically known to be errors from
     * validating the location. Yes, this expression is redundant; it is
     * intentional.
     */
    if ((result == VDO_SUCCESS)
        || ((result != VDO_OUT_OF_RANGE) && (result != VDO_BAD_MAPPING))) {
      return result;
    }
  }

  // Log the corruption even if we wind up ignoring it for write VIOs,
  // converting all cases to VDO_BAD_MAPPING.
  logErrorWithStringError(VDO_BAD_MAPPING, "PBN %" PRIu64
                          " with state %u read from the block map was invalid",
                          mapped.pbn, mapped.state);

  // A read VIO has no option but to report the bad mapping--reading
  // zeros would be hiding known data loss.
  if (isReadDataVIO(dataVIO)) {
    return VDO_BAD_MAPPING;
  }

  // A write VIO only reads this mapping to decref the old block. Treat
  // this as an unmapped entry rather than fail the write.
  clearMappedLocation(dataVIO);
  return VDO_SUCCESS;
}

/**
 * This callback is registered in getMappedBlockAsync().
 **/
static void getMappingFromFetchedPage(VDOCompletion *completion)
{
  if (completion->result != VDO_SUCCESS) {
    finishProcessingPage(completion, completion->result);
    return;
  }

  const BlockMapPage *page   = dereferenceReadableVDOPage(completion);
  int                 result = ASSERT(page != NULL, "page available");
  if (result != VDO_SUCCESS) {
    finishProcessingPage(completion, result);
    return;
  }

  DataVIO             *dataVIO  = asDataVIO(completion->parent);
  BlockMapTreeSlot    *treeSlot = &dataVIO->treeLock.treeSlots[0];
  const BlockMapEntry *entry    = &page->entries[treeSlot->blockMapSlot.slot];

  result = setMappedEntry(dataVIO, entry);
  finishProcessingPage(completion, result);
}

/**
 * This callback is registered in putMappedBlockAsync().
 **/
static void putMappingInFetchedPage(VDOCompletion *completion)
{
  if (completion->result != VDO_SUCCESS) {
    finishProcessingPage(completion, completion->result);
    return;
  }

  BlockMapPage *page   = dereferenceWritableVDOPage(completion);
  int           result = ASSERT(page != NULL, "page available");
  if (result != VDO_SUCCESS) {
    finishProcessingPage(completion, result);
    return;
  }

  DataVIO *dataVIO = asDataVIO(completion->parent);
  BlockMapPageContext *context = getVDOPageCompletionContext(completion);
  SequenceNumber oldLock = context->recoveryLock;
  updateBlockMapPage(page, dataVIO, dataVIO->newMapped.pbn,
                     dataVIO->newMapped.state, &context->recoveryLock);
  markCompletedVDOPageDirty(completion, oldLock, context->recoveryLock);
  finishProcessingPage(completion, VDO_SUCCESS);
}

/**********************************************************************/
void getMappedBlockAsync(DataVIO *dataVIO)
{
  if (dataVIO->treeLock.treeSlots[0].blockMapSlot.pbn == ZERO_BLOCK) {
    // We know that the block map page for this LBN has not been allocated,
    // so the block must be unmapped.
    clearMappedLocation(dataVIO);
    continueDataVIO(dataVIO, VDO_SUCCESS);
    return;
  }

  setupMappedBlock(dataVIO, false, getMappingFromFetchedPage);
}

/**********************************************************************/
void putMappedBlockAsync(DataVIO *dataVIO)
{
  setupMappedBlock(dataVIO, true, putMappingInFetchedPage);
}

/**********************************************************************/
BlockMapStatistics getBlockMapStatistics(BlockMap *map)
{
  BlockMapStatistics stats;
  memset(&stats, 0, sizeof(BlockMapStatistics));

  for (ZoneCount zone = 0; zone < map->zoneCount; zone++) {
    const AtomicPageCacheStatistics *atoms
      = getVDOPageCacheStatistics(map->zones[zone].pageCache);
    stats.dirtyPages      += atomicLoad64(&atoms->counts.dirtyPages);
    stats.cleanPages      += atomicLoad64(&atoms->counts.cleanPages);
    stats.freePages       += atomicLoad64(&atoms->counts.freePages);
    stats.failedPages     += atomicLoad64(&atoms->counts.failedPages);
    stats.incomingPages   += atomicLoad64(&atoms->counts.incomingPages);
    stats.outgoingPages   += atomicLoad64(&atoms->counts.outgoingPages);

    stats.cachePressure   += atomicLoad64(&atoms->cachePressure);
    stats.readCount       += atomicLoad64(&atoms->readCount);
    stats.writeCount      += atomicLoad64(&atoms->writeCount);
    stats.failedReads     += atomicLoad64(&atoms->failedReads);
    stats.failedWrites    += atomicLoad64(&atoms->failedWrites);
    stats.reclaimed       += atomicLoad64(&atoms->reclaimed);
    stats.readOutgoing    += atomicLoad64(&atoms->readOutgoing);
    stats.foundInCache    += atomicLoad64(&atoms->foundInCache);
    stats.discardRequired += atomicLoad64(&atoms->discardRequired);
    stats.waitForPage     += atomicLoad64(&atoms->waitForPage);
    stats.fetchRequired   += atomicLoad64(&atoms->fetchRequired);
    stats.pagesLoaded     += atomicLoad64(&atoms->pagesLoaded);
    stats.pagesSaved      += atomicLoad64(&atoms->pagesSaved);
    stats.flushCount      += atomicLoad64(&atoms->flushCount);
  }

  return stats;
}
