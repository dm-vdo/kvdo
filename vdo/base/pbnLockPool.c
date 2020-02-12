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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/pbnLockPool.c#6 $
 */

#include "pbnLockPool.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "ringNode.h"
#include "pbnLock.h"

/**
 * Unused (idle) PBN locks are kept in a ring. Just like in a malloc
 * implementation, the lock structure is unused memory, so we can save a bit
 * of space (and not pollute the lock structure proper) by using a union to
 * overlay the lock structure with the free list.
 **/
typedef union {
	/** Only used while locks are in the pool */
	RingNode node;
	/** Only used while locks are not in the pool */
	struct pbn_lock lock;
} idle_pbn_lock;

/**
 * The lock pool is little more than the memory allocated for the locks.
 **/
struct pbn_lock_pool {
	/** The number of locks allocated for the pool */
	size_t capacity;
	/** The number of locks currently borrowed from the pool */
	size_t borrowed;
	/** A ring containing all idle PBN lock instances */
	RingNode idle_ring;
	/** The memory for all the locks allocated by this pool */
	idle_pbn_lock locks[];
};

/**********************************************************************/
int make_pbn_lock_pool(size_t capacity, struct pbn_lock_pool **pool_ptr)
{
	struct pbn_lock_pool *pool;
	int result = ALLOCATE_EXTENDED(struct pbn_lock_pool, capacity,
				       idle_pbn_lock, __func__, &pool);
	if (result != VDO_SUCCESS) {
		return result;
	}

	pool->capacity = capacity;
	pool->borrowed = capacity;
	initializeRing(&pool->idle_ring);

	size_t i;
	for (i = 0; i < capacity; i++) {
		struct pbn_lock *lock = &pool->locks[i].lock;
		return_pbn_lock_to_pool(pool, &lock);
	}

	*pool_ptr = pool;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_pbn_lock_pool(struct pbn_lock_pool **pool_ptr)
{
	if (*pool_ptr == NULL) {
		return;
	}

	struct pbn_lock_pool *pool = *pool_ptr;
	ASSERT_LOG_ONLY(pool->borrowed == 0,
			"All PBN locks must be returned to the pool before it is freed, but %zu locks are still on loan",
			pool->borrowed);
	FREE(pool);
	*pool_ptr = NULL;
}

/**********************************************************************/
int borrow_pbn_lock_from_pool(struct pbn_lock_pool *pool,
			      pbn_lock_type type,
			      struct pbn_lock **lock_ptr)
{
	if (pool->borrowed >= pool->capacity) {
		return logErrorWithStringError(VDO_LOCK_ERROR,
					       "no free PBN locks left to borrow");
	}
	pool->borrowed += 1;

	RingNode *idle_node = popRingNode(&pool->idle_ring);
	// The lock was zeroed when it was placed in the pool, but the
	// overlapping ring pointers are non-zero after a pop.
	memset(idle_node, 0, sizeof(*idle_node));

	STATIC_ASSERT(offsetof(idle_pbn_lock, node) ==
		      offsetof(idle_pbn_lock, lock));
	struct pbn_lock *lock = (struct pbn_lock *)idle_node;
	initialize_pbn_lock(lock, type);

	*lock_ptr = lock;
	return VDO_SUCCESS;
}

/**********************************************************************/
void return_pbn_lock_to_pool(struct pbn_lock_pool *pool,
			     struct pbn_lock **lock_ptr)
{
	// Take what should be the last lock reference from the caller
	struct pbn_lock *lock = *lock_ptr;
	*lock_ptr = NULL;

	// A bit expensive, but will promptly catch some use-after-free errors.
	memset(lock, 0, sizeof(*lock));

	RingNode *idle_node = (RingNode *)lock;
	initializeRing(idle_node);
	pushRingNode(&pool->idle_ring, idle_node);

	ASSERT_LOG_ONLY(pool->borrowed > 0,
			"shouldn't return more than borrowed");
	pool->borrowed -= 1;
}
