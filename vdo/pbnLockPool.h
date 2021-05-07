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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/pbnLockPool.h#8 $
 */

#ifndef PBN_LOCK_POOL_H
#define PBN_LOCK_POOL_H

#include "pbnLock.h"
#include "types.h"

struct pbn_lock_pool;

/**
 * Create a new PBN lock pool and all the lock instances it can loan out.
 *
 * @param [in]  capacity    The number of PBN locks to allocate for the pool
 * @param [out] pool_ptr    A pointer to receive the new pool
 *
 * @return a VDO_SUCCESS or an error code
 **/
int __must_check
make_vdo_pbn_lock_pool(size_t capacity, struct pbn_lock_pool **pool_ptr);

/**
 * Free a PBN lock pool null out the reference to it. This also frees all all
 * the PBN locks it allocated, so the caller must ensure that all locks have
 * been returned to the pool.
 *
 * @param [in,out] pool_ptr  The reference to the lock pool to free
 **/
void free_vdo_pbn_lock_pool(struct pbn_lock_pool **pool_ptr);

/**
 * Borrow a PBN lock from the pool and initialize it with the provided type.
 * Pools do not grow on demand or allocate memory, so this will fail if the
 * pool is empty. Borrowed locks are still associated with this pool and must
 * be returned to only this pool.
 *
 * @param [in]  pool      The pool from which to borrow
 * @param [in]  type      The type with which to initialize the lock
 * @param [out] lock_ptr  A pointer to receive the borrowed lock
 *
 * @return VDO_SUCCESS, or VDO_LOCK_ERROR if the pool is empty
 **/
int __must_check
borrow_vdo_pbn_lock_from_pool(struct pbn_lock_pool *pool,
			      enum pbn_lock_type type,
			      struct pbn_lock **lock_ptr);

/**
 * Return to the pool a lock that was borrowed from it, and null out the
 * caller's reference to it. It must be the last live reference, as if the
 * memory were being freed (the lock memory will re-initialized or zeroed).
 *
 * @param [in]     pool      The pool from which the lock was borrowed
 * @param [in,out] lock_ptr  The last reference to the lock being returned
 **/
void return_vdo_pbn_lock_to_pool(struct pbn_lock_pool *pool,
				 struct pbn_lock **lock_ptr);

#endif // PBN_LOCK_POOL_H
