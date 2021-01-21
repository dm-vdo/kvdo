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
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/base/pbnLock.c#11 $
 */

#include "pbnLock.h"

#include "logger.h"

#include "blockAllocator.h"
#include "packedReferenceBlock.h"

struct pbn_lock_implementation {
	enum pbn_lock_type type;
	const char *name;
	const char *release_reason;
};

/**
 * This array must have an entry for every pbn_lock_type value.
 **/
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
	[VIO_COMPRESSED_WRITE_LOCK] = {
		.type = VIO_COMPRESSED_WRITE_LOCK,
		.name = "compressed write",
		.release_reason = "failed compression",
	},
	[VIO_BLOCK_MAP_WRITE_LOCK] = {
		.type = VIO_BLOCK_MAP_WRITE_LOCK,
		.name = "block map write",
		.release_reason = "block map write",
	},
};

/**********************************************************************/
static inline bool has_lock_type(const struct pbn_lock *lock,
				 enum pbn_lock_type type)
{
	return (lock->implementation == &LOCK_IMPLEMENTATIONS[type]);
}

/**********************************************************************/
bool is_pbn_read_lock(const struct pbn_lock *lock)
{
	return has_lock_type(lock, VIO_READ_LOCK);
}

/**********************************************************************/
static inline void set_pbn_lock_type(struct pbn_lock *lock,
				     enum pbn_lock_type type)
{
	lock->implementation = &LOCK_IMPLEMENTATIONS[type];
}

/**********************************************************************/
void initialize_pbn_lock(struct pbn_lock *lock, enum pbn_lock_type type)
{
	lock->holder_count = 0;
	set_pbn_lock_type(lock, type);
}

/**********************************************************************/
void downgrade_pbn_write_lock(struct pbn_lock *lock)
{
	ASSERT_LOG_ONLY(!is_pbn_read_lock(lock),
			"PBN lock must not already have been downgraded");
	ASSERT_LOG_ONLY(!has_lock_type(lock, VIO_BLOCK_MAP_WRITE_LOCK),
			"must not downgrade block map write locks");
	ASSERT_LOG_ONLY(lock->holder_count == 1,
			"PBN write lock should have one holder but has %u",
			lock->holder_count);
	if (has_lock_type(lock, VIO_WRITE_LOCK)) {
		// data_vio write locks are downgraded in place--the writer
		// retains the hold on the lock. They've already had a single
		// incRef journaled.
		lock->increment_limit = MAXIMUM_REFERENCE_COUNT - 1;
	} else {
		// Compressed block write locks are downgraded when they are
		// shared with all their hash locks. The writer is releasing
		// its hold on the lock.
		lock->holder_count = 0;
		lock->increment_limit = MAXIMUM_REFERENCE_COUNT;
	}
	set_pbn_lock_type(lock, VIO_READ_LOCK);
}

/**********************************************************************/
bool claim_pbn_lock_increment(struct pbn_lock *lock)
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

/**********************************************************************/
void assign_provisional_reference(struct pbn_lock *lock)
{
	ASSERT_LOG_ONLY(!lock->has_provisional_reference,
			"lock does not have a provisional reference");
	lock->has_provisional_reference = true;
}

/**********************************************************************/
void unassign_provisional_reference(struct pbn_lock *lock)
{
	lock->has_provisional_reference = false;
}

/**********************************************************************/
void release_provisional_reference(struct pbn_lock *lock,
				   physical_block_number_t locked_pbn,
				   struct block_allocator *allocator)
{
	if (has_provisional_reference(lock)) {
		release_block_reference(allocator,
					locked_pbn,
					lock->implementation->release_reason);
		unassign_provisional_reference(lock);
	}
}
