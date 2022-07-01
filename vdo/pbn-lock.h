/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
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
	VIO_READ_LOCK,
	VIO_WRITE_LOCK,
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

void vdo_initialize_pbn_lock(struct pbn_lock *lock, enum pbn_lock_type type);

bool __must_check vdo_is_pbn_read_lock(const struct pbn_lock *lock);

void
vdo_downgrade_pbn_write_lock(struct pbn_lock *lock, bool compressed_write);

bool __must_check vdo_claim_pbn_lock_increment(struct pbn_lock *lock);

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

void vdo_assign_pbn_lock_provisional_reference(struct pbn_lock *lock);

void vdo_unassign_pbn_lock_provisional_reference(struct pbn_lock *lock);

void
vdo_release_pbn_lock_provisional_reference(struct pbn_lock *lock,
					   physical_block_number_t locked_pbn,
					   struct block_allocator *allocator);

#endif /* PBN_LOCK_H */
