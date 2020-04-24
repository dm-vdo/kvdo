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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/pbnLock.h#8 $
 */

#ifndef PBN_LOCK_H
#define PBN_LOCK_H

#include "atomic.h"
#include "types.h"

/**
 * The type of a PBN lock.
 **/
typedef enum {
	VIO_READ_LOCK = 0,
	VIO_WRITE_LOCK,
	VIO_COMPRESSED_WRITE_LOCK,
	VIO_BLOCK_MAP_WRITE_LOCK,
} pbn_lock_type;

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
	 * For read locks, the number of DataVIOs that have tried to claim one
	 * of the available increments during the lifetime of the lock. Each
	 * claim will first increment this counter, so it can exceed the
	 * increment limit.
	 **/
	Atomic32 increments_claimed;
};

/**
 * Initialize a pbn_lock.
 *
 * @param lock  The lock to initialize
 * @param type  The type of the lock
 **/
void initialize_pbn_lock(struct pbn_lock *lock, pbn_lock_type type);

/**
 * Check whether a pbn_lock is a read lock.
 *
 * @param lock  The lock to check
 *
 * @return <code>true</code> if the lock is a read lock
 **/
bool __must_check is_pbn_read_lock(const struct pbn_lock *lock);

/**
 * Downgrade a PBN write lock to a PBN read lock. The lock holder count is
 * cleared and the caller is responsible for setting the new count.
 *
 * @param lock  The PBN write lock to downgrade
 **/
void downgrade_pbn_write_lock(struct pbn_lock *lock);

/**
 * Try to claim one of the available reference count increments on a read
 * lock. Claims may be attempted from any thread. A claim is only valid until
 * the PBN lock is released.
 *
 * @param lock  The PBN read lock from which to claim an increment
 *
 * @return <code>true</code> if the claim succeeded, guaranteeing one
 *         increment can be made without overflowing the PBN's reference count
 **/
bool __must_check claim_pbn_lock_increment(struct pbn_lock *lock);

/**
 * Check whether a PBN lock has a provisional reference.
 *
 * @param lock  The PBN lock
 **/
static inline bool has_provisional_reference(struct pbn_lock *lock)
{
	return ((lock != NULL) && lock->has_provisional_reference);
}

/**
 * Inform a PBN lock that it is responsible for a provisional reference.
 *
 * @param lock  The PBN lock
 **/
void assign_provisional_reference(struct pbn_lock *lock);

/**
 * Inform a PBN lock that it is no longer responsible for a provisional
 * reference.
 *
 * @param lock  The PBN lock
 **/
void unassign_provisional_reference(struct pbn_lock *lock);

/**
 * If the lock is responsible for a provisional reference, release that
 * reference. This method is called when the lock is released.
 *
 * @param lock        The lock
 * @param locked_pbn  The PBN covered by the lock
 * @param allocator   The block allocator from which to release the reference
 **/
void release_provisional_reference(struct pbn_lock *lock,
				   physical_block_number_t locked_pbn,
				   struct block_allocator *allocator);

#endif /* PBN_LOCK_H */
