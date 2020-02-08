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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/physicalZone.h#6 $
 */

#ifndef PHYSICAL_ZONE_H
#define PHYSICAL_ZONE_H

#include "pbnLock.h"
#include "types.h"

/**
 * Create a physical zone.
 *
 * @param [in]  vdo         The vdo to which the zone will belong
 * @param [in]  zoneNumber  The number of the zone to create
 * @param [out] zonePtr     A pointer to hold the new physical_zone
 *
 * @return VDO_SUCCESS or an error code
 **/
int makePhysicalZone(struct vdo            *vdo,
                     ZoneCount              zoneNumber,
                     struct physical_zone **zonePtr)
  __attribute__((warn_unused_result));

/**
 * Free a physical zone and null out the reference to it.
 *
 * @param zonePtr  A pointer to the zone to free
 **/
void freePhysicalZone(struct physical_zone **zonePtr);

/**
 * Get the zone number of a physical zone.
 *
 * @param zone  The zone
 *
 * @return The number of the zone
 **/
ZoneCount getPhysicalZoneNumber(const struct physical_zone *zone)
  __attribute__((warn_unused_result));

/**
 * Get the ID of a physical zone's thread.
 *
 * @param zone  The zone
 *
 * @return The zone's thread ID
 **/
ThreadID getPhysicalZoneThreadID(const struct physical_zone *zone)
  __attribute__((warn_unused_result));

/**
 * Get the block allocator from a physical zone.
 *
 * @param zone  The zone
 *
 * @return The zone's allocator
 **/
struct block_allocator *getBlockAllocator(const struct physical_zone *zone)
  __attribute__((warn_unused_result));

/**
 * Get the lock on a PBN if one exists.
 *
 * @param zone  The physical zone responsible for the PBN
 * @param pbn   The physical block number whose lock is desired
 *
 * @return The lock or NULL if the PBN is not locked
 **/
struct pbn_lock *getPBNLock(struct physical_zone *zone, PhysicalBlockNumber pbn)
  __attribute__((warn_unused_result));

/**
 * Attempt to lock a physical block in the zone responsible for it. If the PBN
 * is already locked, the existing lock will be returned. Otherwise, a new
 * lock instance will be borrowed from the pool, initialized, and returned.
 * The lock owner will be NULL for a new lock acquired by the caller, who is
 * responsible for setting that field promptly. The lock owner will be
 * non-NULL when there is already an existing lock on the PBN.
 *
 * @param [in]  zone     The physical zone responsible for the PBN
 * @param [in]  pbn      The physical block number to lock
 * @param [in]  type     The type with which to initialize a new lock
 * @param [out] lockPtr  A pointer to receive the lock, existing or new
 *
 * @return VDO_SUCCESS or an error
 **/
int attemptPBNLock(struct physical_zone  *zone,
                   PhysicalBlockNumber    pbn,
                   pbn_lock_type          type,
                   struct pbn_lock      **lockPtr)
  __attribute__((warn_unused_result));

/**
 * Release a physical block lock if it is held, return it to the lock pool,
 * and null out the caller's reference to it. It must be the last live
 * reference, as if the memory were being freed (the lock memory will
 * re-initialized or zeroed).
 *
 * @param [in]     zone       The physical zone in which the lock was obtained
 * @param [in]     lockedPBN  The physical block number to unlock
 * @param [in,out] lockPtr    The last reference to the lock being released
 **/
void releasePBNLock(struct physical_zone *zone,
                    PhysicalBlockNumber   lockedPBN,
                    struct pbn_lock     **lockPtr);

/**
 * Dump information about a physical zone to the log for debugging.
 *
 * @param zone   The zone to dump
 **/
void dumpPhysicalZone(const struct physical_zone *zone);

#endif // PHYSICAL_ZONE_H
