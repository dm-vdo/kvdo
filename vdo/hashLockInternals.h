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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/hashLockInternals.h#17 $
 */

#ifndef HASH_LOCK_INTERNALS_H
#define HASH_LOCK_INTERNALS_H

#include <linux/list.h>

#include "completion.h"
#include "types.h"
#include "uds.h"
#include "waitQueue.h"

enum hash_lock_state {
	/** State for locks that are not in use or are being initialized. */
	HASH_LOCK_INITIALIZING = 0,

	// This is the sequence of states typically used on the non-dedupe path.
	HASH_LOCK_QUERYING,
	HASH_LOCK_WRITING,
	HASH_LOCK_UPDATING,

	// The remaining states are typically used on the dedupe path in this
	// order.
	HASH_LOCK_LOCKING,
	HASH_LOCK_VERIFYING,
	HASH_LOCK_DEDUPING,
	HASH_LOCK_UNLOCKING,

	// XXX This is a temporary state denoting a lock which is sending VIOs
	// back to the old dedupe and vioWrite pathways. It won't be in the
	// final version of VDOSTORY-190.
	HASH_LOCK_BYPASSING,

	/**
	 * Terminal state for locks returning to the pool. Must be last both
	 * because it's the final state, and also because it's used to count
	 * the states.
	 **/
	HASH_LOCK_DESTROYING,
};

struct hash_lock {
	/** The block hash covered by this lock */
	struct uds_chunk_name hash;

	/**
	 * When the lock is unused, this list entry allows the lock to be
	 * pooled
	 */
	struct list_head pool_node;

	/**
	 * A list containing the data VIOs sharing this lock, all having the
	 * same chunk name and data block contents, linked by their
	 * hash_lock_node fields.
	 **/
	struct list_head duplicate_ring;

	/** The number of data_vios sharing this lock instance */
	vio_count_t reference_count;

	/** The maximum value of reference_count in the lifetime of this lock */
	vio_count_t max_references;

	/** The current state of this lock */
	enum hash_lock_state state;

	/** True if the UDS index should be updated with new advice */
	bool update_advice;

	/** True if the advice has been verified to be a true duplicate */
	bool verified;

	/**
	 * True if the lock has already accounted for an initial verification
	 */
	bool verify_counted;

	/** True if this lock is registered in the lock map (cleared on
	 * rollover)
	 */
	bool registered;

	/**
	 * If verified is false, this is the location of a possible duplicate.
	 * If verified is true, is is the verified location of a true duplicate.
	 **/
	struct zoned_pbn duplicate;

	/** The PBN lock on the block containing the duplicate data */
	struct pbn_lock *duplicate_lock;

	/** The data_vio designated to act on behalf of the lock */
	struct data_vio *agent;

	/**
	 * Other data_vios with data identical to the agent who are currently
	 * waiting for the agent to get the information they all need to
	 * deduplicate--either against each other, or against an existing
	 * duplicate on disk.
	 **/
	struct wait_queue waiters;
};

/**
 * Initialize a hash_lock instance which has been newly allocated.
 *
 * @param lock  The lock to initialize
 **/
static inline void initialize_hash_lock(struct hash_lock *lock)
{
	INIT_LIST_HEAD(&lock->pool_node);
	INIT_LIST_HEAD(&lock->duplicate_ring);
	initialize_wait_queue(&lock->waiters);
}

/**
 * Get the string representation of a hash lock state.
 *
 * @param state  The hash lock state
 *
 * @return The short string representing the state
 **/
const char * __must_check get_hash_lock_state_name(enum hash_lock_state state);

#endif // HASH_LOCK_INTERNALS_H
