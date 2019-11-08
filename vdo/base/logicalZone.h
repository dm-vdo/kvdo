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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/logicalZone.h#6 $
 */

#ifndef LOGICAL_ZONE_H
#define LOGICAL_ZONE_H

#include "adminState.h"
#include "intMap.h"
#include "types.h"

/**
 * Get a logical zone by number.
 *
 * @param zones       A set of logical zones
 * @param zoneNumber  The number of the zone to get
 *
 * @return The requested zone
 **/
LogicalZone *getLogicalZone(LogicalZones *zones, ZoneCount zoneNumber)
  __attribute__((warn_unused_result));

/**
 * Create a set of logical zones.
 *
 * @param [in]  vdo       The VDO to which the zones will belong
 * @param [out] zonesPtr  A pointer to hold the new zones
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeLogicalZones(VDO *vdo, LogicalZones **zonesPtr)
  __attribute__((warn_unused_result));

/**
 * Free a set of logical zones and null out the reference to it.
 *
 * @param zonePtr  A pointer to the zone to free
 **/
void freeLogicalZones(LogicalZones **zonePtr);

/**
 * Drain a set of logical zones.
 *
 * @param zones       The logical zones to suspend
 * @param operation   The type of drain to perform
 * @param completion  The object to notify when the zones are suspended
 **/
void drainLogicalZones(LogicalZones   *zones,
                       AdminStateCode  operation,
                       VDOCompletion  *completion);

/**
 * Resume a set of logical zones.
 *
 * @param zones   The logical zones to resume
 * @param parent  The object to notify when the zones have resumed
 **/
void resumeLogicalZones(LogicalZones *zones, VDOCompletion *parent);

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
struct block_map_zone *getBlockMapForZone(const LogicalZone *zone)
  __attribute__((warn_unused_result));

/**
 * Get the logical lock map for this zone.
 *
 * @param zone  The zone
 *
 * @return The logical lock map for the zone
 **/
struct int_map *getLBNLockMap(const LogicalZone *zone)
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
 * Get the selector for deciding which physical zone should be allocated from
 * next for activities in a logical zone.
 *
 * @param zone  The logical zone of the operation which needs an allocation
 *
 * @return The allocation selector for this zone
 **/
struct allocation_selector *getAllocationSelector(LogicalZone *zone)
  __attribute__((warn_unused_result));

/**
 * Dump information about a logical zone to the log for debugging, in a
 * thread-unsafe fashion.
 *
 * @param zone   The zone to dump
 **/
void dumpLogicalZone(const LogicalZone *zone);

#endif // LOGICAL_ZONE_H
