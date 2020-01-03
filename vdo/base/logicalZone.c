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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/logicalZone.c#15 $
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

struct logical_zone {
  /** The completion for flush notifications */
  struct vdo_completion       completion;
  /** The owner of this zone */
  struct logical_zones       *zones;
  /** Which logical zone this is */
  ZoneCount                   zoneNumber;
  /** The thread id for this zone */
  ThreadID                    threadID;
  /** In progress operations keyed by LBN */
  struct int_map             *lbnOperations;
  /** The logical to physical map */
  struct block_map_zone      *blockMapZone;
  /** The current flush generation */
  SequenceNumber              flushGeneration;
  /** The oldest active generation in this zone */
  SequenceNumber              oldestActiveGeneration;
  /** The number of IOs in the current flush generation */
  BlockCount                  iosInFlushGeneration;
  /**
   * The oldest locked generation in this zone (an atomic copy of
   *                          oldestActiveGeneration)
   **/
  Atomic64                    oldestLockedGeneration;
  /** The youngest generation of the current notification */
  SequenceNumber              notificationGeneration;
  /** Whether a notification is in progress */
  bool                        notifying;
  /** The queue of active data write VIOs */
  RingNode                    writeVIOs;
  /** The administrative state of the zone */
  struct admin_state          state;
  /** The selector for determining which physical zone to allocate from */
  struct allocation_selector *selector;
};

struct logical_zones {
  /** The vdo whose zones these are */
  struct vdo            *vdo;
  /** The manager for administrative actions */
  struct action_manager *manager;
  /** The number of zones */
  ZoneCount              zoneCount;
  /** The logical zones themselves */
  struct logical_zone    zones[];
};

/**
 * Convert a generic vdo_completion to a logical_zone.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a logical_zone
 **/
static struct logical_zone *asLogicalZone(struct vdo_completion *completion)
{
  STATIC_ASSERT(offsetof(struct logical_zone, completion) == 0);
  assertCompletionType(completion->type, GENERATION_FLUSHED_COMPLETION);
  return (struct logical_zone *) completion;
}

/**********************************************************************/
struct logical_zone *getLogicalZone(struct logical_zones *zones,
                                    ZoneCount             zoneNumber)
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
 * @param zones       The logical_zones to which this zone belongs
 * @param zoneNumber  The logical_zone's index
 **/
static int initializeZone(struct logical_zones *zones, ZoneCount zoneNumber)
{
  struct logical_zone *zone = &zones->zones[zoneNumber];
  zone->zones = zones;
  int result = makeIntMap(LOCK_MAP_CAPACITY, 0, &zone->lbnOperations);
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct vdo *vdo = zones->vdo;
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
int makeLogicalZones(struct vdo *vdo, struct logical_zones **zonesPtr)
{
  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  if (threadConfig->logicalZoneCount == 0) {
    return VDO_SUCCESS;
  }

  struct logical_zones *zones;
  int result = ALLOCATE_EXTENDED(struct logical_zones,
                                 threadConfig->logicalZoneCount,
                                 struct logical_zone, __func__, &zones);
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

  result = make_action_manager(zones->zoneCount, getThreadIDForZone,
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
void freeLogicalZones(struct logical_zones **zonesPtr)
{
  struct logical_zones *zones = *zonesPtr;
  if (zones == NULL) {
    return;
  }

  free_action_manager(&zones->manager);

  for (ZoneCount index = 0; index < zones->zoneCount; index++) {
    struct logical_zone *zone = &zones->zones[index];
    freeAllocationSelector(&zone->selector);
    destroyEnqueueable(&zone->completion);
    freeIntMap(&zone->lbnOperations);
  }

  FREE(zones);
  *zonesPtr = NULL;
}

/**********************************************************************/
static inline void assertOnZoneThread(struct logical_zone *zone,
                                      const char          *what)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == zone->threadID),
                  "%s() called on correct thread", what);
}

/**
 * Check whether this zone has drained.
 *
 * @param zone  The zone to check
 **/
static void checkForDrainComplete(struct logical_zone *zone)
{
  if (!isDraining(&zone->state) || zone->notifying
      || !isRingEmpty(&zone->writeVIOs)) {
    return;
  }

  finishDraining(&zone->state);
}

/**
 * Initiate a drain.
 *
 * Implements AdminInitiator.
 **/
static void initiateDrain(struct admin_state *state)
{
  checkForDrainComplete(container_of(state, struct logical_zone, state));
}

/**
 * Drain a logical zone.
 *
 * <p>Implements ZoneAction.
 **/
static void drainLogicalZone(void                  *context,
                             ZoneCount              zoneNumber,
                             struct vdo_completion *parent)
{
  struct logical_zone *zone = getLogicalZone(context, zoneNumber);
  startDraining(&zone->state,
                get_current_manager_operation(zone->zones->manager),
                parent, initiateDrain);
}

/**********************************************************************/
void drainLogicalZones(struct logical_zones  *zones,
                       AdminStateCode         operation,
                       struct vdo_completion *parent)
{
  schedule_operation(zones->manager, operation, NULL, drainLogicalZone, NULL,
                     parent);
}

/**
 * Resume a logical zone.
 *
 * <p>Implements ZoneAction.
 **/
static void resumeLogicalZone(void                  *context,
                              ZoneCount              zoneNumber,
                              struct vdo_completion *parent)
{
  struct logical_zone *zone = getLogicalZone(context, zoneNumber);
  finishCompletion(parent, resumeIfQuiescent(&zone->state));
}

/**********************************************************************/
void resumeLogicalZones(struct logical_zones  *zones,
                        struct vdo_completion *parent)
{
  schedule_operation(zones->manager, ADMIN_STATE_RESUMING, NULL,
                     resumeLogicalZone, NULL, parent);
}

/**********************************************************************/
ThreadID getLogicalZoneThreadID(const struct logical_zone *zone)
{
  return zone->threadID;
}

/**********************************************************************/
struct block_map_zone *getBlockMapForZone(const struct logical_zone *zone)
{
  return zone->blockMapZone;
}

/**********************************************************************/
struct int_map *getLBNLockMap(const struct logical_zone *zone)
{
  return zone->lbnOperations;
}

/**********************************************************************/
struct logical_zone *getNextLogicalZone(const struct logical_zone *zone)
{
  return getLogicalZone(zone->zones, zone->zoneNumber + 1);
}

/**
 * Convert a RingNode to a data_vio.
 *
 * @param ringNode The RingNode to convert
 *
 * @return The data_vio which owns the RingNode
 **/
static inline struct data_vio *dataVIOFromRingNode(RingNode *ringNode)
{
  return (struct data_vio *) ((byte *) ringNode - offsetof(struct data_vio,
                                                           writeNode));
}

/**
 * Update the oldest active generation. If it has changed, update the
 * atomic copy as well.
 *
 * @param zone  The zone
 *
 * @return <code>true</code> if the oldest active generation has changed
 **/
static bool updateOldestActiveGeneration(struct logical_zone *zone)
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
void incrementFlushGeneration(struct logical_zone *zone,
                              SequenceNumber       expectedGeneration)
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
SequenceNumber getOldestLockedGeneration(const struct logical_zone *zone)
{
  return (SequenceNumber) atomicLoad64(&zone->oldestLockedGeneration);
}

/**********************************************************************/
int acquireFlushGenerationLock(struct data_vio *dataVIO)
{
  struct logical_zone *zone = dataVIO->logical.zone;
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
static void
attemptGenerationCompleteNotification(struct vdo_completion *completion);

/**
 * Notify the flush that at least one generation no longer has active VIOs.
 * This callback is registered in attemptGenerationCompleteNotification().
 *
 * @param completion  The zone completion
 **/
static void notifyFlusher(struct vdo_completion *completion)
{
  struct logical_zone *zone = asLogicalZone(completion);
  completeFlushes(zone->zones->vdo->flusher);
  launchCallback(completion, attemptGenerationCompleteNotification,
                 zone->threadID);
}

/**
 * Notify the flusher if some generation no longer has active VIOs.
 *
 * @param completion  The zone completion
 **/
static void
attemptGenerationCompleteNotification(struct vdo_completion *completion)
{
  struct logical_zone *zone = asLogicalZone(completion);
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
void releaseFlushGenerationLock(struct data_vio *dataVIO)
{
  struct logical_zone *zone = dataVIO->logical.zone;
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
                  "data_vio releasing lock on generation %" PRIu64
                  " is not older than oldest active generation %llu",
                  dataVIO->flushGeneration, zone->oldestActiveGeneration);

  if (!updateOldestActiveGeneration(zone) || zone->notifying) {
    return;
  }

  attemptGenerationCompleteNotification(&zone->completion);
}

/**********************************************************************/
struct allocation_selector *getAllocationSelector(struct logical_zone *zone)
{
  return zone->selector;
}

/**********************************************************************/
void dumpLogicalZone(const struct logical_zone *zone)
{
  logInfo("logical_zone %u", zone->zoneNumber);
  logInfo("  flushGeneration=%llu oldestActiveGeneration=%" PRIu64
          " oldestLockedGeneration=%llu notificationGeneration=%" PRIu64
          " notifying=%s iosInCurrentGeneration=%llu",
          zone->flushGeneration, zone->oldestActiveGeneration,
          relaxedLoad64(&zone->oldestLockedGeneration),
          zone->notificationGeneration, boolToString(zone->notifying),
          zone->iosInFlushGeneration);
}
