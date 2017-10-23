/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/hashZone.h#1 $
 */

#ifndef HASH_ZONE_H
#define HASH_ZONE_H

#include "uds.h"
#include "types.h"

/**
 * Create a hash zone.
 *
 * @param [in]  vdo         The VDO to which the zone will belong
 * @param [in]  zoneNumber  The number of the zone to create
 * @param [out] zonePtr     A pointer to hold the new HashZone
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeHashZone(VDO *vdo, ZoneCount zoneNumber, HashZone **zonePtr)
  __attribute__((warn_unused_result));

/**
 * Free a hash zone and null out the reference to it.
 *
 * @param zonePtr  A pointer to the zone to free
 **/
void freeHashZone(HashZone **zonePtr);

/**
 * Get the zone number of a hash zone.
 *
 * @param zone  The zone
 *
 * @return The number of the zone
 **/
ZoneCount getHashZoneNumber(const HashZone *zone)
  __attribute__((warn_unused_result));

/**
 * Get the ID of a hash zone's thread.
 *
 * @param zone  The zone
 *
 * @return The zone's thread ID
 **/
ThreadID getHashZoneThreadID(const HashZone *zone)
  __attribute__((warn_unused_result));

/**
 * Get the lock for the hash (chunk name) of the data in a DataVIO, or if one
 * does not exist, initialize a new lock for the hash and register it in the
 * zone. This must only be called in the correct thread for the zone.
 *
 * @param [in]  zone     The zone responsible for the hash
 * @param [in]  hash     The hash to lock
 * @param [out] lockPtr  A pointer to receive the hash lock
 *
 * @return VDO_SUCCESS or an error code
 **/
int acquireHashLockFromZone(HashZone            *zone,
                            const UdsChunkName  *hash,
                            HashLock           **lockPtr)
  __attribute__((warn_unused_result));

/**
 * Return a hash lock to the zone it was borrowed from, remove it from the
 * zone's lock map, returning it to the pool, and nulling out the reference to
 * it. This must only be called when the lock has been completely released,
 * and only in the correct thread for the zone.
 *
 * @param [in]     zone     The zone from which the lock was borrowed
 * @param [in,out] lockPtr  The lock that is no longer in use
 **/
void returnHashLockToZone(HashZone *zone, HashLock **lockPtr);

#endif // HASH_ZONE_H
