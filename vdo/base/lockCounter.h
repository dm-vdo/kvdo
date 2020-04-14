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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/lockCounter.h#6 $
 */

#ifndef LOCK_COUNTER_H
#define LOCK_COUNTER_H

#include "completion.h"
#include "types.h"

/**
 * A lock_counter provides a set of shared reference count locks which is safe
 * across multiple zones with a minimum of cross-thread synchronization
 * operations. For each lock in the set, it maintains a set of per-zone lock
 * counts, and a single, atomic count of the number of zones holding locks.
 * Whenever a zone's individual counter for a lock goes from 0 to 1, the
 * zone count for that lock is incremented. Whenever a zone's individual
 * counter for a lock goes from 1 to 0, the zone count for that lock is
 * decremented. If the zone count goes to 0, and the lock counter's
 * completion is not in use, the completion is launched to inform the counter's
 * owner that some lock has been released. It is the owner's responsibility to
 * check for which locks have been released, and to inform the lock counter
 * that it has received the notification by calling acknowledge_unlock().
 **/

/**
 * Create a lock counter.
 *
 * @param [in]  layer             The physical layer of the VDO
 * @param [in]  parent            The parent to notify when the lock count goes
 *                                to zero
 * @param [in]  callback          The function to call when the lock count goes
 *                                to zero
 * @param [in]  thread_id         The id of thread on which to run the callback
 * @param [in]  logical_zones     The total number of logical zones
 * @param [in]  physical_zones    The total number of physical zones
 * @param [in]  locks             The number of locks
 * @param [out] lock_counter_ptr  A pointer to hold the new counter
 *
 * @return VDO_SUCCESS or an error
 **/
int make_lock_counter(PhysicalLayer *layer, void *parent, vdo_action callback,
		      ThreadID thread_id, ZoneCount logical_zones,
		      ZoneCount physical_zones, block_count_t locks,
		      struct lock_counter **lock_counter_ptr)
	__attribute__((warn_unused_result));

/**
 * Destroy a lock counter and NULL out the reference to it.
 *
 * @param lock_counter_ptr  A pointer to the lock counter reference to free
 **/
void free_lock_counter(struct lock_counter **lock_counter_ptr);

/**
 * Check whether a lock is locked for a zone type. If the recovery journal has
 * a lock on the lock number, both logical and physical zones are considered
 * locked.
 *
 * @param lock_counter  The set of locks to check
 * @param lock_number   The lock to check
 * @param zone_type     The type of the zone
 *
 * @return <code>true</code> if the specified lock has references (is locked)
 **/
bool is_locked(struct lock_counter *lock_counter, block_count_t lock_number,
	       zone_type zone_type) __attribute__((warn_unused_result));

/**
 * Initialize the value of the journal zone's counter for a given lock. This
 * must be called from the journal zone.
 *
 * @param counter      The counter to initialize
 * @param lock_number  Which lock to initialize
 * @param value        The value to set
 **/
void initialize_lock_count(struct lock_counter *counter,
			   block_count_t lock_number,
			   uint16_t value);

/**
 * Acquire a reference to a given lock in the specified zone. This method must
 * not be used from the journal zone.
 *
 * @param counter      The lock_counter
 * @param lock_number  Which lock to increment
 * @param zone_type    The type of the zone acquiring the reference
 * @param zoneID       The ID of the zone acquiring the reference
 **/
void acquire_lock_count_reference(struct lock_counter *counter,
				  block_count_t lock_number,
				  zone_type zone_type,
				  ZoneCount zoneID);

/**
 * Release a reference to a given lock in the specified zone. This method
 * must not be used from the journal zone.
 *
 * @param counter      The lock_counter
 * @param lock_number  Which lock to increment
 * @param zone_type    The type of the zone releasing the reference
 * @param zone_id      The ID of the zone releasing the reference
 **/
void release_lock_count_reference(struct lock_counter *counter,
				  block_count_t lock_number,
				  zone_type zone_type,
				  ZoneCount zone_id);

/**
 * Release a single journal zone reference from the journal zone. This method
 * must be called from the journal zone.
 *
 * @param counter      The counter from which to release a reference
 * @param lock_number  The lock from which to release a reference
 **/
void release_journal_zone_reference(struct lock_counter *counter,
				    block_count_t lock_number);

/**
 * Release a single journal zone reference from any zone. This method shouldn't
 * be called from the journal zone as it would be inefficient; use
 * release_journal_zone_reference() instead.
 *
 * @param counter      The counter from which to release a reference
 * @param lock_number  The lock from which to release a reference
 **/
void
release_journal_zone_reference_from_other_zone(struct lock_counter *counter,
					       block_count_t lock_number);

/**
 * Inform a lock counter that an unlock notification was received by the
 * caller.
 *
 * @param counter  The counter to inform
 **/
void acknowledge_unlock(struct lock_counter *counter);

#endif // LOCK_COUNTER_H
