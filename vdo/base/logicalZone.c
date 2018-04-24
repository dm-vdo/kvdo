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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/logicalZone.c#2 $
 */

#include "logicalZone.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "atomic.h"
#include "blockMap.h"
#include "completion.h"
#include "constants.h"
#include "dataVIO.h"
#include "flush.h"
#include "intMap.h"
#include "vdoInternal.h"

struct logicalZone {
  /** The completion for flush notifications */
  VDOCompletion   completion;
  /** Which logical zone this is */
  ZoneCount       zoneNumber;
  /** The next zone in the iteration list */
  LogicalZone    *nextZone;
  /** The per thread data for this zone */
  ThreadData     *threadData;
  /** In progress operations keyed by LBN */
  IntMap         *lbnOperations;
  /** The logical to physical map */
  BlockMapZone   *blockMapZone;
  /** The current flush generation */
  SequenceNumber  flushGeneration;
  /** The oldest active generation in this zone */
  SequenceNumber  oldestActiveGeneration;
  /** The number of IOs in the current flush generation */
  BlockCount      iosInFlushGeneration;
  /**
   * The oldest locked generation in this zone (an atomic copy of
   * oldestActiveGeneration)
   **/
  Atomic64        oldestLockedGeneration;
  /** The youngest generation of the current notification */
  SequenceNumber  notificationGeneration;
  /** Whether a notification is in progress */
  bool            notifying;
  /** The queue of active data write VIOs */
  RingNode        writeVIOs;
  /** The VDO */
  VDO            *vdo;
  /** The completion waiting for the zone to close */
  VDOCompletion  *closeCompletion;
  /** Whether a close has been requested */
  bool            closeRequested;
};

/**
 * Convert a generic VDOCompletion to a LogicalZone.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a LogicalZone
 **/
static LogicalZone *asLogicalZone(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(LogicalZone, completion) == 0);
  assertCompletionType(completion->type, GENERATION_FLUSHED_COMPLETION);
  return (LogicalZone *) completion;
}

/**********************************************************************/
int makeLogicalZone(VDO          *vdo,
                    ZoneCount     zoneNumber,
                    LogicalZone  *nextZone,
                    LogicalZone **zonePtr)
{
  LogicalZone *zone;
  int result = ALLOCATE(1, LogicalZone, __func__, &zone);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeIntMap(LOCK_MAP_CAPACITY, 0, &zone->lbnOperations);
  if (result != VDO_SUCCESS) {
    freeLogicalZone(&zone);
    return result;
  }

  result = initializeEnqueueableCompletion(&zone->completion,
                                           GENERATION_FLUSHED_COMPLETION,
                                           vdo->layer);
  if (result != VDO_SUCCESS) {
    freeLogicalZone(&zone);
    return result;
  }

  ThreadID threadID  = getLogicalZoneThread(getThreadConfig(vdo), zoneNumber);
  zone->zoneNumber   = zoneNumber;
  zone->nextZone     = nextZone;
  zone->threadData   = &vdo->threadData[threadID];
  zone->blockMapZone = getBlockMapZone(vdo->blockMap, zoneNumber);
  zone->vdo          = vdo;
  initializeRing(&zone->writeVIOs);
  atomicStore64(&zone->oldestLockedGeneration, 0);

  *zonePtr = zone;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeLogicalZone(LogicalZone **logicalZonePtr)
{
  if (*logicalZonePtr == NULL) {
    return;
  }

  LogicalZone *zone = *logicalZonePtr;
  destroyEnqueueable(&zone->completion);
  freeIntMap(&zone->lbnOperations);
  FREE(zone);
  *logicalZonePtr = NULL;
}

/**********************************************************************/
static inline void assertOnZoneThread(LogicalZone *zone, const char *what)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == zone->threadData->threadID),
                  "%s() called on correct thread", what);
}

/**
 * Close the next zone. This callback is registered in checkForClosure().
 *
 * @param completion  The zone which has just closed as a completion
 **/
static void closeLogicalZoneCallback(VDOCompletion *completion)
{
  LogicalZone *zone = asLogicalZone(completion);
  closeLogicalZone(getNextLogicalZone(zone), completion->parent);
}

/**
 * Check whether this zone is closed.
 *
 * @param zone  The zone to check
 **/
static void checkForClosure(LogicalZone *zone)
{
  if ((zone->closeCompletion == NULL) || zone->notifying
      || !isRingEmpty(&zone->writeVIOs)) {
    return;
  }

  VDOCompletion *closeCompletion = zone->closeCompletion;
  zone->closeCompletion          = NULL;

  if (zone->nextZone == NULL) {
    // This is the last zone, so finish the close completion.
    finishCompletion(closeCompletion, VDO_SUCCESS);
    return;
  }

  // This is not the last zone, so pass the close completion on to the next.
  zone->notifying = true;
  launchCallbackWithParent(&zone->completion, closeLogicalZoneCallback,
                           getLogicalZoneThreadID(zone->nextZone),
                           closeCompletion);
}

/**********************************************************************/
void closeLogicalZone(LogicalZone *zone, VDOCompletion *completion)
{
  assertOnZoneThread(zone, __func__);
  if (zone->closeCompletion != NULL) {
    finishCompletion(completion, VDO_COMPONENT_BUSY);
    return;
  }

  zone->closeRequested  = true;
  zone->closeCompletion = completion;
  checkForClosure(zone);
}

/**********************************************************************/
ThreadID getLogicalZoneThreadID(const LogicalZone *zone)
{
  return zone->threadData->threadID;
}

/**********************************************************************/
BlockMapZone *getBlockMapForZone(const LogicalZone *zone)
{
  return zone->blockMapZone;
}

/**********************************************************************/
IntMap *getLBNLockMap(const LogicalZone *zone)
{
  return zone->lbnOperations;
}

/**********************************************************************/
LogicalZone *getNextLogicalZone(const LogicalZone *zone)
{
  return zone->nextZone;
}

/**
 * Convert a RingNode to a DataVIO.
 *
 * @param ringNode The RingNode to convert
 *
 * @return The DataVIO which owns the RingNode
 **/
static inline DataVIO *dataVIOFromRingNode(RingNode *ringNode)
{
  return (DataVIO *) ((byte *) ringNode - offsetof(DataVIO, writeNode));
}

/**
 * Update the oldest active generation. If it has changed, update the
 * atomic copy as well.
 *
 * @param zone  The zone
 *
 * @return <code>true</code> if the oldest active generation has changed
 **/
static bool updateOldestActiveGeneration(LogicalZone *zone)
{
  SequenceNumber currentOldest = zone->oldestActiveGeneration;
  if (isRingEmpty(&zone->writeVIOs)) {
    zone->oldestActiveGeneration = zone->flushGeneration;
  } else {
    zone->oldestActiveGeneration
      = dataVIOFromRingNode(zone->writeVIOs.next)->flushGeneration;
  }

  if (zone->oldestActiveGeneration == currentOldest) {
    return false;
  }

  atomicStore64(&zone->oldestLockedGeneration, zone->oldestActiveGeneration);
  return true;
}

/**********************************************************************/
void incrementFlushGeneration(LogicalZone    *zone,
                              SequenceNumber  expectedGeneration)
{
  assertOnZoneThread(zone, __func__);
  ASSERT_LOG_ONLY((zone->flushGeneration == expectedGeneration),
                  "logical zone %u flush generation %" PRIu64
                  " should be %" PRIu64 " before increment",
                  zone->zoneNumber, zone->flushGeneration,
                  expectedGeneration);

  zone->flushGeneration++;
  zone->iosInFlushGeneration = 0;
  updateOldestActiveGeneration(zone);
}

/**********************************************************************/
SequenceNumber getOldestLockedGeneration(const LogicalZone *zone)
{
  return (SequenceNumber) atomicLoad64(&zone->oldestLockedGeneration);
}

/**********************************************************************/
int acquireFlushGenerationLock(DataVIO *dataVIO)
{
  LogicalZone *zone = dataVIO->logical.zone;
  assertOnZoneThread(zone, __func__);
  if (zone->closeRequested && (zone->closeCompletion == NULL)) {
    return VDO_SHUTTING_DOWN;
  }

  dataVIO->flushGeneration = zone->flushGeneration;
  pushRingNode(&zone->writeVIOs, &dataVIO->writeNode);
  dataVIO->hasFlushGenerationLock = true;
  zone->iosInFlushGeneration++;
  return VDO_SUCCESS;
}

/**********************************************************************/
static void attemptGenerationCompleteNotification(VDOCompletion *completion);

/**
 * Notify the flush that at least one generation no longer has active VIOs.
 * This callback is registered in attemptGenerationCompleteNotification().
 *
 * @param completion  The zone completion
 **/
static void notifyFlusher(VDOCompletion *completion)
{
  LogicalZone *zone = asLogicalZone(completion);
  completeFlushes(zone->vdo->flusher);
  launchCallback(completion, attemptGenerationCompleteNotification,
                 zone->threadData->threadID);
}

/**
 * Notify the flusher if some generation no longer has active VIOs.
 *
 * @param completion  The zone completion
 **/
static void attemptGenerationCompleteNotification(VDOCompletion *completion)
{
  LogicalZone *zone = asLogicalZone(completion);
  assertOnZoneThread(zone, __func__);
  if (zone->oldestActiveGeneration <= zone->notificationGeneration) {
    zone->notifying = false;
    checkForClosure(zone);
    return;
  }

  zone->notifying              = true;
  zone->notificationGeneration = zone->oldestActiveGeneration;
  launchCallback(&zone->completion, notifyFlusher,
                 getFlusherThreadID(zone->vdo->flusher));
}

/**********************************************************************/
void releaseFlushGenerationLock(DataVIO *dataVIO)
{
  LogicalZone *zone = dataVIO->logical.zone;
  assertOnZoneThread(zone, __func__);
  if (isRingEmpty(&dataVIO->writeNode)) {
    // This VIO never got a lock, either because it is a read, or because
    // we are in read-only mode.
    ASSERT_LOG_ONLY(!dataVIO->hasFlushGenerationLock,
                    "hasFlushGenerationLock false for VIO not on active list");
    return;
  }

  unspliceRingNode(&dataVIO->writeNode);
  dataVIO->hasFlushGenerationLock = false;
  ASSERT_LOG_ONLY(zone->oldestActiveGeneration <= dataVIO->flushGeneration,
                  "DataVIO releasing lock on generation %" PRIu64
                  " is not older than oldest active generation %" PRIu64,
                  dataVIO->flushGeneration, zone->oldestActiveGeneration);

  if (!updateOldestActiveGeneration(zone) || zone->notifying) {
    return;
  }

  attemptGenerationCompleteNotification(&zone->completion);
}

/**********************************************************************/
void dumpLogicalZone(const LogicalZone *zone)
{
  logInfo("LogicalZone %u", zone->zoneNumber);
  logInfo("  flushGeneration=%" PRIu64 " oldestActiveGeneration=%" PRIu64
          " oldestLockedGeneration=%" PRIu64 " notificationGeneration=%" PRIu64
          " notifying=%s iosInCurrentGeneration=%" PRIu64,
          zone->flushGeneration, zone->oldestActiveGeneration,
          relaxedLoad64(&zone->oldestLockedGeneration),
          zone->notificationGeneration, boolToString(zone->notifying),
          zone->iosInFlushGeneration);
}
