/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/pbnLockPool.h#1 $
 */

#ifndef PBN_LOCK_POOL_H
#define PBN_LOCK_POOL_H

#include "pbnLock.h"
#include "types.h"

typedef struct pbnLockPool PBNLockPool;

/**
 * Create a new PBN lock pool and all the lock instances it can loan out.
 *
 * @param [in]  capacity   The number of PBN locks to allocate for the pool
 * @param [out] poolPtr    A pointer to receive the new pool
 *
 * @return a VDO_SUCCESS or an error code
 **/
int makePBNLockPool(size_t capacity, PBNLockPool **poolPtr)
  __attribute__((warn_unused_result));

/**
 * Free a PBN lock pool null out the reference to it. This also frees all all
 * the PBN locks it allocated, so the caller must ensure that all locks have
 * been returned to the pool.
 *
 * @param [in,out] poolPtr  The reference to the lock pool to free
 **/
void freePBNLockPool(PBNLockPool **poolPtr);

/**
 * Borrow a PBN lock from the pool and initialize it with the provided type.
 * Pools do not grow on demand or allocate memory, so this will fail if the
 * pool is empty. Borrowed locks are still associated with this pool and must
 * be returned to only this pool.
 *
 * @param [in]  pool     The pool from which to borrow
 * @param [in]  type     The type with which to initialize the lock
 * @param [out] lockPtr  A pointer to receive the borrowed lock
 *
 * @return VDO_SUCCESS, or VDO_LOCK_ERROR if the pool is empty
 **/
int borrowPBNLockFromPool(PBNLockPool  *pool,
                          PBNLockType   type,
                          PBNLock     **lockPtr)
  __attribute__((warn_unused_result));

/**
 * Return to the pool a lock that was borrowed from it, and null out the
 * caller's reference to it. It must be the last live reference, as if the
 * memory were being freed (the lock memory will re-initialized or zeroed).
 *
 * @param [in]     pool     The pool from which the lock was borrowed
 * @param [in,out] lockPtr  The last reference to the lock being returned
 **/
void returnPBNLockToPool(PBNLockPool *pool, PBNLock **lockPtr);

#endif // PBN_LOCK_POOL_H
