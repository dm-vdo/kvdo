/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/logicalZone.c#7 $
 */

#include "logicalZone.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "actionManager.h"
#include "adminState.h"
#include "allocationSelector.h"
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
  VDOCompletion       completion;
  /** The owner of this zone */
  LogicalZones       *zones;
  /** Which logical zone this is */
  ZoneCount           zoneNumber;
  /** The thread id for this zone */
  ThreadID            threadID;
  /** In progress operations keyed by LBN */
  struct int_map     *lbnOperations;
  /** The logical to physical map */
  BlockMapZone       *blockMapZone;
  /** The current flush generation */
  SequenceNumber      flushGeneration;
  /** The oldest active generation in this zone */
  SequenceNumber      oldestActiveGeneration;
  /** The number of IOs in the current flush generation */
  BlockCount          iosInFlushGeneration;
  /**
   * The oldest locked generation in this zone (an atomic copy of
   *                  oldestActiveGeneration)
   **/
  Atomic64            oldestLockedGeneration;
  /** The youngest generation of the current notification */
  SequenceNumber      notificationGeneration;
  /** Whether a notification is in progress */
  bool                notifying;
  /** The queue of active data write VIOs */
  RingNode            writeVIOs;
  /** The administrative state of the zone */
  struct admin_state  state;
  /** The selector for determining which physical zone to allocate from */
  AllocationSelector *selector;
};

struct logicalZones {
  /** The VDO whose zones these are */
  VDO           *vdo;
  /** The manager for administrative actions */
  ActionManager *manager;
  /** The number of zones */
  ZoneCount      zoneCount;
  /** The logical zones themselves */
  LogicalZone    zones[];
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
LogicalZone *getLogicalZone(LogicalZones *zones, ZoneCount zoneNumber)
{
  return (zoneNumber < zones->zoneCount) ? &zones->zones[zoneNumber] : NULL;
}

/**
 * Implements ZoneThreadGetter
 **/
static ThreadID getThreadIDForZone(void *context, ZoneCount zoneNumber)
{
  return getLogicalZoneThreadID(getLogicalZone(context, zoneNumber));
}

/**
 * Initialize a logical zone.
 *
 * @param zones       The LogicalZones to which this zone belongs
 * @param zoneNumber  The LogicalZone's index
 **/
static int initializeZone(LogicalZones *zones, ZoneCount zoneNumber)
{
  LogicalZone *zone   = &zones->zones[zoneNumber];
  zone->zones         = zones;
  int          result = makeIntMap(LOCK_MAP_CAPACITY, 0, &zone->lbnOperations);
  if (result != VDO_SUCCESS) {
    return result;
  }

  VDO *vdo = zones->vdo;
  result = initializeEnqueueableCompletion(&zone->completion,
                                           GENERATION_FLUSHED_COMPLETION,
                                           vdo->layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  zone->zoneNumber   = zoneNumber;
  zone->threadID     = getLogicalZoneThread(getThreadConfig(vdo),
                                            zoneNumber);
  zone->blockMapZone = getBlockMapZone(vdo->blockMap, zoneNumber);
  initializeRing(&zone->writeVIOs);
  atomicStore64(&zone->oldestLockedGeneration, 0);

  return makeAllocationSelector(getThreadConfig(vdo)->physicalZoneCount,
                                zone->threadID, &zone->selector);
}

/**********************************************************************/
int makeLogicalZones(VDO *vdo, LogicalZones **zonesPtr)
{
  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  if (threadConfig->logicalZoneCount == 0) {
    return VDO_SUCCESS;
  }

  LogicalZones *zones;
  int result = ALLOCATE_EXTENDED(LogicalZones, threadConfig->logicalZoneCount,
                                 LogicalZone, __func__, &zones);
  if (result != VDO_SUCCESS) {
    return result;
  }

  zones->vdo = vdo;
  zones->zoneCount = threadConfig->logicalZoneCount;
  for (ZoneCount zone = 0; zone < threadConfig->logicalZoneCount; zone++) {
    result = initializeZone(zones, zone);
    if (result != VDO_SUCCESS) {
      freeLogicalZones(&zones);
      return result;
    }
  }

  result = makeActionManager(zones->zoneCount, getThreadIDForZone,
                             getAdminThread(threadConfig), zones, NULL,
                             vdo->layer, &zones->manager);
  if (result != VDO_SUCCESS) {
    freeLogicalZones(&zones);
    return result;
  }

  *zonesPtr = zones;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeLogicalZones(LogicalZones **zonesPtr)
{
  LogicalZones *zones = *zonesPtr;
  if (zones == NULL) {
    return;
  }

  freeActionManager(&zones->manager);

  for (ZoneCount index = 0; index < zones->zoneCount; index++) {
    LogicalZone *zone = &zones->zones[index];
    freeAllocationSelector(&zone->selector);
    destroyEnqueueable(&zone->completion);
    freeIntMap(&zone->lbnOperations);
  }

  FREE(zones);
  *zonesPtr = NULL;
}

/**********************************************************************/
static inline void assertOnZoneThread(LogicalZone *zone, const char *what)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == zone->threadID),
                  "%s() called on correct thread", what);
}

/**
 * Check whether this zone has drained.
 *
 * @param zone  The zone to check
 **/
static void checkForDrainComplete(LogicalZone *zone)
{
  if (!isDraining(&zone->state) || zone->notifying
      || !isRingEmpty(&zone->writeVIOs)) {
    return;
  }

  finishDraining(&zone->state);
}

/**
 * Drain a logical zone.
 *
 * <p>Implements ZoneAction.
 **/
static void drainLogicalZone(void          *context,
                             ZoneCount      zoneNumber,
                             VDOCompletion *parent)
{
  LogicalZone    *zone      = getLogicalZone(context, zoneNumber);
  AdminStateCode  operation = getCurrentManagerOperation(zone->zones->manager);
  if (startDraining(&zone->state, operation, parent)) {
    checkForDrainComplete(zone);
  }
}

/**********************************************************************/
void drainLogicalZones(LogicalZones   *zones,
                       AdminStateCode  operation,
                       VDOCompletion  *parent)
{
  scheduleOperation(zones->manager, operation, NULL, drainLogicalZone, NULL,
                    parent);
}

/**
 * Resume a logical zone.
 *
 * <p>Implements ZoneAction.
 **/
static void resumeLogicalZone(void          *context,
                              ZoneCount      zoneNumber,
                              VDOCompletion *parent)
{
  LogicalZone *zone = getLogicalZone(context, zoneNumber);
  if (startResuming(&zone->state, ADMIN_STATE_RESUMING, parent)) {
    finishResuming(&zone->state);
  }
}

/**********************************************************************/
void resumeLogicalZones(LogicalZones *zones, VDOCompletion *parent)
{
  scheduleOperation(zones->manager, ADMIN_STATE_RESUMING, NULL,
                    resumeLogicalZone, NULL, parent);
}

/**********************************************************************/
ThreadID getLogicalZoneThreadID(const LogicalZone *zone)
{
  return zone->threadID;
}

/**********************************************************************/
BlockMapZone *getBlockMapForZone(const LogicalZone *zone)
{
  return zone->blockMapZone;
}

/**********************************************************************/
struct int_map *getLBNLockMap(const LogicalZone *zone)
{
  return zone->lbnOperations;
}

/**********************************************************************/
LogicalZone *getNextLogicalZone(const LogicalZone *zone)
{
  return getLogicalZone(zone->zones, zone->zoneNumber + 1);
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
                  " should be %llu before increment",
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
  if (!isNormal(&zone->state)) {
    return VDO_INVALID_ADMIN_STATE;
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
  completeFlushes(zone->zones->vdo->flusher);
  launchCallback(completion, attemptGenerationCompleteNotification,
                 zone->threadID);
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
    checkForDrainComplete(zone);
    return;
  }

  zone->notifying              = true;
  zone->notificationGeneration = zone->oldestActiveGeneration;
  launchCallback(&zone->completion, notifyFlusher,
                 getFlusherThreadID(zone->zones->vdo->flusher));
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
                  " is not older than oldest active generation %llu",
                  dataVIO->flushGeneration, zone->oldestActiveGeneration);

  if (!updateOldestActiveGeneration(zone) || zone->notifying) {
    return;
  }

  attemptGenerationCompleteNotification(&zone->completion);
}

/**********************************************************************/
AllocationSelector *getAllocationSelector(LogicalZone *zone)
{
  return zone->selector;
}

/**********************************************************************/
void dumpLogicalZone(const LogicalZone *zone)
{
  logInfo("LogicalZone %u", zone->zoneNumber);
  logInfo("  flushGeneration=%llu oldestActiveGeneration=%" PRIu64
          " oldestLockedGeneration=%llu notificationGeneration=%" PRIu64
          " notifying=%s iosInCurrentGeneration=%llu",
          zone->flushGeneration, zone->oldestActiveGeneration,
          relaxedLoad64(&zone->oldestLockedGeneration),
          zone->notificationGeneration, boolToString(zone->notifying),
          zone->iosInFlushGeneration);
}
