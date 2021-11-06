/*
 * Copyright Red Hat
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
 */

#ifndef PBN_LOCK_H
#define PBN_LOCK_H

#include <linux/atomic.h>

#include "kernel-types.h"
#include "types.h"

/**
 * The type of a PBN lock.
 **/
enum pbn_lock_type {
	VIO_READ_LOCK = 0,
	VIO_WRITE_LOCK,
	VIO_COMPRESSED_WRITE_LOCK,
	VIO_BLOCK_MAP_WRITE_LOCK,
};

struct pbn_lock_implementation;

/**
 * A PBN lock.
 **/
struct pbn_lock {
	/** The implementation of the lock */
	const struct pbn_lock_implementation *implementation;

	/** The number of VIOs holding or sharing this lock */
	vio_count_t holder_count;
	/**
	 * The number of compressed block writers holding a share of this lock
	 * while they are acquiring a reference to the PBN.
	 **/
	uint8_t fragment_locks;

	/**
	 * Whether the locked PBN has been provisionally referenced on behalf of
	 * the lock holder.
	 **/
	bool has_provisional_reference;

	/**
	 * For read locks, the number of references that were known to be
	 * available on the locked block at the time the lock was acquired.
	 **/
	uint8_t increment_limit;

	/**
	 * For read locks, the number of data_vios that have tried to claim one
	 * of the available increments during the lifetime of the lock. Each
	 * claim will first increment this counter, so it can exceed the
	 * increment limit.
	 **/
	atomic_t increments_claimed;
};

void initialize_vdo_pbn_lock(struct pbn_lock *lock, enum pbn_lock_type type);

bool __must_check is_vdo_pbn_read_lock(const struct pbn_lock *lock);

void downgrade_vdo_pbn_write_lock(struct pbn_lock *lock);

bool __must_check claim_vdo_pbn_lock_increment(struct pbn_lock *lock);

/**
 * Check whether a PBN lock has a provisional reference.
 *
 * @param lock  The PBN lock
 **/
static inline bool
vdo_pbn_lock_has_provisional_reference(struct pbn_lock *lock)
{
	return ((lock != NULL) && lock->has_provisional_reference);
}

void assign_vdo_pbn_lock_provisional_reference(struct pbn_lock *lock);

void unassign_vdo_pbn_lock_provisional_reference(struct pbn_lock *lock);

void
release_vdo_pbn_lock_provisional_reference(struct pbn_lock *lock,
					   physical_block_number_t locked_pbn,
					   struct block_allocator *allocator);

#endif /* PBN_LOCK_H */
