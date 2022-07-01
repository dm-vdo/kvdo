// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "pbn-lock.h"

#include "logger.h"
#include "permassert.h"

#include "block-allocator.h"
#include "packed-reference-block.h"

struct pbn_lock_implementation {
	enum pbn_lock_type type;
	const char *name;
	const char *release_reason;
};

/*
 * This array must have an entry for every pbn_lock_type value.
 */
static const struct pbn_lock_implementation LOCK_IMPLEMENTATIONS[] = {
	[VIO_READ_LOCK] = {
		.type = VIO_READ_LOCK,
		.name = "read",
		.release_reason = "candidate duplicate",
	},
	[VIO_WRITE_LOCK] = {
		.type = VIO_WRITE_LOCK,
		.name = "write",
		.release_reason = "newly allocated",
	},
	[VIO_BLOCK_MAP_WRITE_LOCK] = {
		.type = VIO_BLOCK_MAP_WRITE_LOCK,
		.name = "block map write",
		.release_reason = "block map write",
	},
};

static inline bool has_lock_type(const struct pbn_lock *lock,
				 enum pbn_lock_type type)
{
	return (lock->implementation == &LOCK_IMPLEMENTATIONS[type]);
}

/**
 * vdo_is_pbn_read_lock() - Check whether a pbn_lock is a read lock.
 * @lock: The lock to check.
 *
 * Return: true if the lock is a read lock.
 */
bool vdo_is_pbn_read_lock(const struct pbn_lock *lock)
{
	return has_lock_type(lock, VIO_READ_LOCK);
}

static inline void set_pbn_lock_type(struct pbn_lock *lock,
				     enum pbn_lock_type type)
{
	lock->implementation = &LOCK_IMPLEMENTATIONS[type];
}

/**
 * vdo_initialize_pbn_lock() - Initialize a pbn_lock.
 * @lock: The lock to initialize.
 * @type: The type of the lock.
 */
void vdo_initialize_pbn_lock(struct pbn_lock *lock, enum pbn_lock_type type)
{
	lock->holder_count = 0;
	set_pbn_lock_type(lock, type);
}

/**
 * vdo_downgrade_pbn_write_lock() - Downgrade a PBN write lock to a
 *                                  PBN read lock.
 * @lock: The PBN write lock to downgrade.
 *
 * The lock holder count is cleared and the caller is responsible for
 * setting the new count.
 */
void vdo_downgrade_pbn_write_lock(struct pbn_lock *lock, bool compressed_write)
{
	ASSERT_LOG_ONLY(!vdo_is_pbn_read_lock(lock),
			"PBN lock must not already have been downgraded");
	ASSERT_LOG_ONLY(!has_lock_type(lock, VIO_BLOCK_MAP_WRITE_LOCK),
			"must not downgrade block map write locks");
	ASSERT_LOG_ONLY(lock->holder_count == 1,
			"PBN write lock should have one holder but has %u",
			lock->holder_count);
	/*
	 * data_vio write locks are downgraded in place--the writer
	 * retains the hold on the lock. If this was a compressed write, the
         * holder has not yet journaled its own inc ref, otherwise, it has.
	 */
	lock->increment_limit = (compressed_write ?
				 MAXIMUM_REFERENCE_COUNT :
				 MAXIMUM_REFERENCE_COUNT - 1);
	set_pbn_lock_type(lock, VIO_READ_LOCK);
}

/**
 * vdo_claim_pbn_lock_increment() - Try to claim one of the available
 *                                  reference count increments on a read lock.
 * @lock: The PBN read lock from which to claim an increment.
 *
 * Claims may be attempted from any thread. A claim is only valid
 * until the PBN lock is released.
 *
 * Return: true if the claim succeeded, guaranteeing one increment can
 *         be made without overflowing the PBN's reference count.
 */
bool vdo_claim_pbn_lock_increment(struct pbn_lock *lock)
{
	/*
	 * Claim the next free reference atomically since hash locks from
	 * multiple hash zone threads might be concurrently deduplicating
	 * against a single PBN lock on compressed block. As long as hitting
	 * the increment limit will lead to the PBN lock being released in a
	 * sane time-frame, we won't overflow a 32-bit claim counter, allowing
	 * a simple add instead of a compare-and-swap.
	 */
	uint32_t claim_number =
		(uint32_t) atomic_add_return(1, &lock->increments_claimed);
	return (claim_number <= lock->increment_limit);
}

/**
 * vdo_assign_pbn_lock_provisional_reference() - Inform a PBN lock that it is
 *                                               responsible for a provisional
 *                                               reference.
 * @lock: The PBN lock.
 */
void vdo_assign_pbn_lock_provisional_reference(struct pbn_lock *lock)
{
	ASSERT_LOG_ONLY(!lock->has_provisional_reference,
			"lock does not have a provisional reference");
	lock->has_provisional_reference = true;
}

/**
 * vdo_unassign_pbn_lock_provisional_reference() - Inform a PBN lock that it
 *                                                 is no longer responsible
 *                                                 for a provisional
 *                                                 reference.
 * @lock: The PBN lock.
 */
void vdo_unassign_pbn_lock_provisional_reference(struct pbn_lock *lock)
{
	lock->has_provisional_reference = false;
}

/**
 * vdo_release_pbn_lock_provisional_reference() - If the lock is responsible
 *                                                for a provisional reference,
 *                                                release that reference.
 * @lock: The lock.
 * @locked_pbn: The PBN covered by the lock.
 * @allocator: The block allocator from which to release the reference.
 *
 * This method is called when the lock is released.
 */
void vdo_release_pbn_lock_provisional_reference(struct pbn_lock *lock,
						physical_block_number_t locked_pbn,
						struct block_allocator *allocator)
{
	if (vdo_pbn_lock_has_provisional_reference(lock)) {
		vdo_release_block_reference(allocator,
					    locked_pbn,
					    lock->implementation->release_reason);
		vdo_unassign_pbn_lock_provisional_reference(lock);
	}
}
