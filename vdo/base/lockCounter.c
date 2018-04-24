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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/lockCounter.c#2 $
 */

#include "lockCounter.h"

#include "atomic.h"
#include "memoryAlloc.h"

/**
 * LockCounter is intended to keep all of the locks for the blocks in the
 * recovery journal. The per-zone counters are all kept in a single array which
 * is arranged by zone (i.e. zone 0's lock 0 is at index 0, zone 0's lock 1 is
 * at index 1, and zone 1's lock 0 is at index 'locks'.  This arrangement is
 * intended to minimize cache-line contention for counters from different
 * zones.
 *
 * The locks are implemented as a single object instead of as a lock counter
 * per lock both to afford this opportunity to reduce cache line contention and
 * also to eliminate the need to have a completion per lock.
 *
 * Lock sets are laid out with the set for recovery journal first, followed by
 * the logical zones, and then the physical zones.
 **/
struct lockCounter {
  /** The completion for notifying the owner of a lock release */
  VDOCompletion  completion;
  /** The number of logical zones which may hold locks */
  ZoneCount      logicalZones;
  /** The number of physical zones which may hold locks */
  ZoneCount      physicalZones;
  /** The number of locks */
  BlockCount     locks;
  /** Whether the lock release notification is in flight */
  AtomicBool     notifying;
  /** The number of logical zones which hold each lock */
  Atomic32      *logicalZoneCounts;
  /** The number of physical zones which hold each lock */
  Atomic32      *physicalZoneCounts;
  /** The per-zone, per-lock counts for the journal zone */
  uint16_t      *journalCounters;
  /** The per-zone, per-lock decrement counts for the journal zone */
  Atomic32      *journalDecrementCounts;
  /** The per-zone, per-lock reference counts for logical zones */
  uint16_t      *logicalCounters;
  /** The per-zone, per-lock reference counts for physical zones */
  uint16_t      *physicalCounters;
};

/**********************************************************************/
int makeLockCounter(PhysicalLayer  *layer,
                    void           *parent,
                    VDOAction       callback,
                    ThreadID        threadID,
                    ZoneCount       logicalZones,
                    ZoneCount       physicalZones,
                    BlockCount      locks,
                    LockCounter   **lockCounterPtr)
{
  LockCounter *lockCounter;

  int result = ALLOCATE(1, LockCounter, __func__, &lockCounter);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(locks, uint16_t, __func__, &lockCounter->journalCounters);
  if (result != VDO_SUCCESS) {
    freeLockCounter(&lockCounter);
    return result;
  }

  result = ALLOCATE(locks, Atomic32, __func__,
                    &lockCounter->journalDecrementCounts);
  if (result != VDO_SUCCESS) {
    freeLockCounter(&lockCounter);
    return result;
  }

  result = ALLOCATE(locks * logicalZones, uint16_t, __func__,
                    &lockCounter->logicalCounters);
  if (result != VDO_SUCCESS) {
    freeLockCounter(&lockCounter);
    return result;
  }

  result = ALLOCATE(locks, Atomic32, __func__,
                    &lockCounter->logicalZoneCounts);
  if (result != VDO_SUCCESS) {
    freeLockCounter(&lockCounter);
    return result;
  }

  result = ALLOCATE(locks * physicalZones, uint16_t, __func__,
                    &lockCounter->physicalCounters);
  if (result != VDO_SUCCESS) {
    freeLockCounter(&lockCounter);
    return result;
  }

  result = ALLOCATE(locks, Atomic32, __func__,
                    &lockCounter->physicalZoneCounts);
  if (result != VDO_SUCCESS) {
    freeLockCounter(&lockCounter);
    return result;
  }

  result = initializeEnqueueableCompletion(&lockCounter->completion,
                                           LOCK_COUNTER_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    freeLockCounter(&lockCounter);
    return result;
  }

  setCallbackWithParent(&lockCounter->completion, callback, threadID, parent);
  lockCounter->logicalZones  = logicalZones;
  lockCounter->physicalZones = physicalZones;
  lockCounter->locks         = locks;
  *lockCounterPtr            = lockCounter;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeLockCounter(LockCounter **lockCounterPtr)
{
  if (*lockCounterPtr == NULL) {
    return;
  }

  LockCounter *lockCounter = *lockCounterPtr;
  destroyEnqueueable(&lockCounter->completion);
  freeVolatile(lockCounter->physicalZoneCounts);
  freeVolatile(lockCounter->logicalZoneCounts);
  freeVolatile(lockCounter->journalDecrementCounts);
  FREE(lockCounter->journalCounters);
  FREE(lockCounter->logicalCounters);
  FREE(lockCounter->physicalCounters);
  FREE(lockCounter);
  *lockCounterPtr = NULL;
}

/**
 * Get a pointer to the zone count for a given lock on a given zone.
 *
 * @param counter     The lock counter
 * @param lockNumber  The lock to get
 * @param zoneType    The zone type whose count is desired
 *
 * @return A pointer to the zone count for the given lock and zone
 **/
static inline Atomic32 *getZoneCountPtr(LockCounter *counter,
                                        BlockCount   lockNumber,
                                        ZoneType     zoneType)
{
  return ((zoneType == ZONE_TYPE_LOGICAL)
          ? &counter->logicalZoneCounts[lockNumber]
          : &counter->physicalZoneCounts[lockNumber]);
}

/**
 * Get the zone counter for a given lock on a given zone.
 *
 * @param counter     The lock counter
 * @param lockNumber  The lock to get
 * @param zoneType    The zone type whose count is desired
 * @param zoneID      The zone index whose count is desired
 *
 * @return The counter for the given lock and zone
 **/
static inline uint16_t *getCounter(LockCounter *counter,
                                   BlockCount   lockNumber,
                                   ZoneType     zoneType,
                                   ZoneCount    zoneID)
{
  BlockCount zoneCounter = (counter->locks * zoneID) + lockNumber;
  if (zoneType == ZONE_TYPE_JOURNAL) {
    return &counter->journalCounters[zoneCounter];
  }

  if (zoneType == ZONE_TYPE_LOGICAL) {
    return &counter->logicalCounters[zoneCounter];
  }

  return &counter->physicalCounters[zoneCounter];
}

/**
 * Check whether the journal zone is locked for a given lock.
 *
 * @param counter     The LockCounter
 * @param lockNumber  The lock to check
 *
 * @return <code>true</code> if the journal zone is locked
 **/
static bool isJournalZoneLocked(LockCounter *counter, BlockCount lockNumber)
{
  uint16_t journalValue
    = *(getCounter(counter, lockNumber, ZONE_TYPE_JOURNAL, 0));
  uint32_t decrements
    = atomicLoad32(&(counter->journalDecrementCounts[lockNumber]));
  ASSERT_LOG_ONLY((decrements <= journalValue),
                  "journal zone lock counter must not underflow");

  return (journalValue != decrements);
}

/**********************************************************************/
bool isLocked(LockCounter *lockCounter,
              BlockCount   lockNumber,
              ZoneType     zoneType)
{
  ASSERT_LOG_ONLY((zoneType != ZONE_TYPE_JOURNAL),
                  "isLocked() called for non-journal zone");
  return (isJournalZoneLocked(lockCounter, lockNumber)
          || (atomicLoad32(getZoneCountPtr(lockCounter, lockNumber, zoneType))
              != 0));
}

/**
 * Check that we are on the journal thread.
 *
 * @param counter  The LockCounter
 * @param caller   The name of the caller (for logging)
 **/
static void assertOnJournalThread(LockCounter *counter, const char *caller)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == counter->completion.callbackThreadID),
                  "%s() called from journal zone", caller);
}

/**********************************************************************/
void initializeLockCount(LockCounter *counter,
                         BlockCount   lockNumber,
                         uint16_t     value)
{
  assertOnJournalThread(counter, __func__);
  uint16_t *journalValue   = getCounter(counter, lockNumber, ZONE_TYPE_JOURNAL,
                                        0);
  Atomic32 *decrementCount = &(counter->journalDecrementCounts[lockNumber]);
  ASSERT_LOG_ONLY((*journalValue == atomicLoad32(decrementCount)),
                  "count to be initialized not in use");

  *journalValue = value;
  atomicStore32(decrementCount, 0);
}

/**********************************************************************/
void acquireLockCountReference(LockCounter *counter,
                               BlockCount   lockNumber,
                               ZoneType     zoneType,
                               ZoneCount    zoneID)
{
  ASSERT_LOG_ONLY((zoneType != ZONE_TYPE_JOURNAL),
                  "invalid lock count increment from journal zone");

  uint16_t *currentValue = getCounter(counter, lockNumber, zoneType, zoneID);
  ASSERT_LOG_ONLY(*currentValue < UINT16_MAX,
                  "increment of lock counter must not overflow");

  if (*currentValue == 0) {
    // This zone is acquiring this lock for the first time.
    atomicAdd32(getZoneCountPtr(counter, lockNumber, zoneType), 1);
  }
  *currentValue += 1;
}

/**
 * Decrement a non-atomic counter.
 *
 * @param counter     The LockCounter
 * @param lockNumber  Which lock to decrement
 * @param zoneType    The type of the zone releasing the reference
 * @param zoneID      The ID of the zone releasing the reference
 *
 * @return The new value of the counter
 **/
static uint16_t releaseReference(LockCounter *counter,
                                 BlockCount   lockNumber,
                                 ZoneType     zoneType,
                                 ZoneCount    zoneID)
{
  uint16_t *currentValue = getCounter(counter, lockNumber, zoneType, zoneID);
  ASSERT_LOG_ONLY((*currentValue >= 1),
                  "decrement of lock counter must not underflow");

  *currentValue -= 1;
  return *currentValue;
}

/**
 * Attempt to notify the owner of this LockCounter that some lock has been
 * released for some zone type. Will do nothing if another notification is
 * already in progress.
 *
 * @param counter  The LockCounter
 **/
static void attemptNotification(LockCounter *counter)
{
  if (compareAndSwapBool(&counter->notifying, false, true)) {
    resetCompletion(&counter->completion);
    invokeCallback(&counter->completion);
  }
}

/**********************************************************************/
void releaseLockCountReference(LockCounter *counter,
                               BlockCount   lockNumber,
                               ZoneType     zoneType,
                               ZoneCount    zoneID)
{
  ASSERT_LOG_ONLY((zoneType != ZONE_TYPE_JOURNAL),
                  "invalid lock count decrement from journal zone");
  if (releaseReference(counter, lockNumber, zoneType, zoneID) != 0) {
    return;
  }

  if (atomicAdd32(getZoneCountPtr(counter, lockNumber, zoneType), -1) == 0) {
    // This zone was the last lock holder of its type, so try to notify the
    // owner.
    attemptNotification(counter);
  }
}

/**********************************************************************/
void releaseJournalZoneReference(LockCounter *counter, BlockCount lockNumber)
{
  assertOnJournalThread(counter, __func__);
  releaseReference(counter, lockNumber, ZONE_TYPE_JOURNAL, 0);
  if (!isJournalZoneLocked(counter, lockNumber)) {
    // The journal zone is not locked, so try to notify the owner.
    attemptNotification(counter);
  }
}

/**********************************************************************/
void releaseJournalZoneReferenceFromOtherZone(LockCounter *counter,
                                              BlockCount   lockNumber)
{
  atomicAdd32(&(counter->journalDecrementCounts[lockNumber]), 1);
}

/**********************************************************************/
void acknowledgeUnlock(LockCounter *counter)
{
  atomicStoreBool(&counter->notifying, false);
}
