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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/pbnLockPool.c#15 $
 */

#include "pbnLockPool.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include <linux/list.h>

#include "pbnLock.h"
#include "statusCodes.h"

/**
 * Unused (idle) PBN locks are kept in a list. Just like in a malloc
 * implementation, the lock structure is unused memory, so we can save a bit
 * of space (and not pollute the lock structure proper) by using a union to
 * overlay the lock structure with the free list.
 **/
typedef union {
	/** Only used while locks are in the pool */
	struct list_head entry;
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
	/** A list containing all idle PBN lock instances */
	struct list_head idle_list;
	/** The memory for all the locks allocated by this pool */
	idle_pbn_lock locks[];
};

/**********************************************************************/
int make_pbn_lock_pool(size_t capacity, struct pbn_lock_pool **pool_ptr)
{
	size_t i;
	struct pbn_lock_pool *pool;
	int result = ALLOCATE_EXTENDED(struct pbn_lock_pool, capacity,
				       idle_pbn_lock, __func__, &pool);
	if (result != VDO_SUCCESS) {
		return result;
	}

	pool->capacity = capacity;
	pool->borrowed = capacity;
	INIT_LIST_HEAD(&pool->idle_list);

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
	struct pbn_lock_pool *pool;

	if (*pool_ptr == NULL) {
		return;
	}

	pool = *pool_ptr;
	ASSERT_LOG_ONLY(pool->borrowed == 0,
			"All PBN locks must be returned to the pool before it is freed, but %zu locks are still on loan",
			pool->borrowed);
	FREE(pool);
	*pool_ptr = NULL;
}

/**********************************************************************/
int borrow_pbn_lock_from_pool(struct pbn_lock_pool *pool,
			      enum pbn_lock_type type,
			      struct pbn_lock **lock_ptr)
{
	int result;
	struct list_head *idle_entry;
	idle_pbn_lock *idle;

	if (pool->borrowed >= pool->capacity) {
		return log_error_strerror(VDO_LOCK_ERROR,
					  "no free PBN locks left to borrow");
	}
	pool->borrowed += 1;

	result = ASSERT(!list_empty(&pool->idle_list),
			"idle list should not be empty if pool not at capacity");
	if (result != VDO_SUCCESS) {
		return result;
	}

	idle_entry = pool->idle_list.prev;
	list_del(idle_entry);
	memset(idle_entry, 0, sizeof(*idle_entry));

	idle = list_entry(idle_entry, idle_pbn_lock, entry);
	initialize_pbn_lock(&idle->lock, type);

	*lock_ptr = &idle->lock;
	return VDO_SUCCESS;
}

/**********************************************************************/
void return_pbn_lock_to_pool(struct pbn_lock_pool *pool,
			     struct pbn_lock **lock_ptr)
{
	idle_pbn_lock *idle;

	// Take what should be the last lock reference from the caller
	struct pbn_lock *lock = *lock_ptr;
	*lock_ptr = NULL;

	// A bit expensive, but will promptly catch some use-after-free errors.
	memset(lock, 0, sizeof(*lock));

	idle = container_of(lock, idle_pbn_lock, lock);
	INIT_LIST_HEAD(&idle->entry);
	list_add_tail(&idle->entry, &pool->idle_list);

	ASSERT_LOG_ONLY(pool->borrowed > 0,
			"shouldn't return more than borrowed");
	pool->borrowed -= 1;
}
