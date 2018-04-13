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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/logicalZone.h#1 $
 */

#ifndef LOGICAL_ZONE_H
#define LOGICAL_ZONE_H

#include "intMap.h"
#include "types.h"

/**
 * Create a logical zone.
 *
 * @param [in]  vdo         The VDO to which the zone will belong
 * @param [in]  zoneNumber  The number of the zone to create
 * @param [in]  nextZone    The next zone in the iteration list
 * @param [out] zonePtr     A pointer to hold the new LogicalZone
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeLogicalZone(VDO          *vdo,
                    ZoneCount     zoneNumber,
                    LogicalZone  *nextZone,
                    LogicalZone **zonePtr)
  __attribute__((warn_unused_result));

/**
 * Free a logical zone and null out the reference to it.
 *
 * @param zonePtr  A pointer to the zone to free
 **/
void freeLogicalZone(LogicalZone **zonePtr);

/**
 * Request that a logical zone close.
 *
 * @param zone        The zone to close
 * @param completion  The object to notify when the zone is closed
 **/
void closeLogicalZone(LogicalZone *zone, VDOCompletion *completion);

/**
 * Get the ID of a logical zone's thread.
 *
 * @param zone  The zone
 *
 * @return The zone's thread ID
 **/
ThreadID getLogicalZoneThreadID(const LogicalZone *zone)
  __attribute__((warn_unused_result));

/**
 * Get the portion of the block map for this zone.
 *
 * @param zone  The zone
 *
 * @return The block map zone
 **/
BlockMapZone *getBlockMapForZone(const LogicalZone *zone)
  __attribute__((warn_unused_result));

/**
 * Get the logical lock map for this zone.
 *
 * @param zone  The zone
 *
 * @return The logical lock map for the zone
 **/
IntMap *getLBNLockMap(const LogicalZone *zone)
  __attribute__((warn_unused_result));

/**
 * Get the next-highest-numbered logical zone, or <code>NULL</code> if the
 * zone is the highest-numbered zone in its VDO.
 *
 * @param zone  The logical zone to query
 *
 * @return The logical zone whose zone number is one greater than the given
 *         zone, or <code>NULL</code> if there is no such zone
 **/
LogicalZone *getNextLogicalZone(const LogicalZone *zone)
  __attribute__((warn_unused_result));

/**
 * Increment the flush generation in a logical zone.
 *
 * @param zone                The logical zone
 * @param expectedGeneration  The expected value of the flush generation
 *                            before the increment
 **/
void incrementFlushGeneration(LogicalZone    *zone,
                              SequenceNumber  expectedGeneration);

/**
 * Get the oldest flush generation which is locked by a logical zone.
 *
 * @param zone   The logical zone
 *
 * @return The oldest generation locked by the zone
 **/
SequenceNumber getOldestLockedGeneration(const LogicalZone *zone)
  __attribute__((warn_unused_result));

/**
 * Acquire the shared lock on a flush generation by a write DataVIO.
 *
 * @param dataVIO   The DataVIO
 *
 * @return VDO_SUCCESS or an error code
 **/
int acquireFlushGenerationLock(DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Release the shared lock on a flush generation held by a write DataVIO. If
 * there are pending flushes, and this DataVIO completes the oldest generation
 * active in this zone, an attempt will be made to finish any flushes which may
 * now be complete.
 *
 * @param dataVIO  The DataVIO whose lock is to be released
 **/
void releaseFlushGenerationLock(DataVIO *dataVIO);

/**
 * Dump information about a logical zone to the log for debugging, in a
 * thread-unsafe fashion.
 *
 * @param zone   The zone to dump
 **/
void dumpLogicalZone(const LogicalZone *zone);

#endif // LOGICAL_ZONE_H
