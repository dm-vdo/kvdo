// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "pbn-lock-pool.h"

#include <linux/list.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "pbn-lock.h"
#include "status-codes.h"

/**
 * union idle_pbn_lock - PBN lock list entries.
 *
 * Unused (idle) PBN locks are kept in a list. Just like in a malloc
 * implementation, the lock structure is unused memory, so we can save a bit
 * of space (and not pollute the lock structure proper) by using a union to
 * overlay the lock structure with the free list.
 */
typedef union {
	/** @entry: Only used while locks are in the pool. */
	struct list_head entry;
	/** @lock: Only used while locks are not in the pool. */
	struct pbn_lock lock;
} idle_pbn_lock;

/**
 * struct pbn_lock_pool - list of PBN locks.
 *
 * The lock pool is little more than the memory allocated for the locks.
 */
struct pbn_lock_pool {
	/** @capacity: The number of locks allocated for the pool. */
	size_t capacity;
	/** @borrowed: The number of locks currently borrowed from the pool. */
	size_t borrowed;
	/** @idle_list: A list containing all idle PBN lock instances. */
	struct list_head idle_list;
	/** @locks: The memory for all the locks allocated by this pool. */
	idle_pbn_lock locks[];
};

/**
 * vdo_make_pbn_lock_pool() - Create a new PBN lock pool and all the lock
 *                            instances it can loan out.
 * @capacity: The number of PBN locks to allocate for the pool.
 * @pool_ptr: A pointer to receive the new pool.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_make_pbn_lock_pool(size_t capacity, struct pbn_lock_pool **pool_ptr)
{
	size_t i;
	struct pbn_lock_pool *pool;
	int result = UDS_ALLOCATE_EXTENDED(struct pbn_lock_pool, capacity,
					   idle_pbn_lock, __func__, &pool);
	if (result != VDO_SUCCESS) {
		return result;
	}

	pool->capacity = capacity;
	pool->borrowed = capacity;
	INIT_LIST_HEAD(&pool->idle_list);

	for (i = 0; i < capacity; i++) {
		vdo_return_pbn_lock_to_pool(pool, &pool->locks[i].lock);
	}

	*pool_ptr = pool;
	return VDO_SUCCESS;
}

/**
 * vdo_free_pbn_lock_pool() - Free a PBN lock pool.
 * @pool: The lock pool to free.
 *
 * This also frees all the PBN locks it allocated, so the caller must ensure
 * that all locks have been returned to the pool.
 */
void vdo_free_pbn_lock_pool(struct pbn_lock_pool *pool)
{
	if (pool == NULL) {
		return;
	}

	ASSERT_LOG_ONLY(pool->borrowed == 0,
			"All PBN locks must be returned to the pool before it is freed, but %zu locks are still on loan",
			pool->borrowed);
	UDS_FREE(pool);
}

/**
 * vdo_borrow_pbn_lock_from_pool() - Borrow a PBN lock from the pool and
 *                                   initialize it with the provided type.
 * @pool: The pool from which to borrow.
 * @type: The type with which to initialize the lock.
 * @lock_ptr:  A pointer to receive the borrowed lock.
 *
 * Pools do not grow on demand or allocate memory, so this will fail if the
 * pool is empty. Borrowed locks are still associated with this pool and must
 * be returned to only this pool.
 *
 * Return: VDO_SUCCESS, or VDO_LOCK_ERROR if the pool is empty.
 */
int vdo_borrow_pbn_lock_from_pool(struct pbn_lock_pool *pool,
				  enum pbn_lock_type type,
				  struct pbn_lock **lock_ptr)
{
	int result;
	struct list_head *idle_entry;
	idle_pbn_lock *idle;

	if (pool->borrowed >= pool->capacity) {
		return uds_log_error_strerror(VDO_LOCK_ERROR,
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
	vdo_initialize_pbn_lock(&idle->lock, type);

	*lock_ptr = &idle->lock;
	return VDO_SUCCESS;
}

/**
 * vdo_return_pbn_lock_to_pool() - Return to the pool a lock that was borrowed
 *                                 from it.
 * @pool: The pool from which the lock was borrowed.
 * @lock: The last reference to the lock being returned.
 *
 * It must be the last live reference, as if the memory were being freed (the
 * lock memory will re-initialized or zeroed).
 */
void vdo_return_pbn_lock_to_pool(struct pbn_lock_pool *pool,
				 struct pbn_lock *lock)
{
	idle_pbn_lock *idle;

	/* A bit expensive, but will promptly catch some use-after-free errors. */
	memset(lock, 0, sizeof(*lock));

	idle = container_of(lock, idle_pbn_lock, lock);
	INIT_LIST_HEAD(&idle->entry);
	list_add_tail(&idle->entry, &pool->idle_list);

	ASSERT_LOG_ONLY(pool->borrowed > 0,
			"shouldn't return more than borrowed");
	pool->borrowed -= 1;
}
