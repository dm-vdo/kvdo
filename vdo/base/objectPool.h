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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/objectPool.h#1 $
 */

#ifndef OBJECT_POOL_H
#define OBJECT_POOL_H

#include "permassert.h"

#include "completion.h"
#include "types.h"
#include "waitQueue.h"

/**
 * Create a new object pool.
 *
 * @param [in]  entryData  The entry data as a single buffer, held onto by
 *                         the pool to make cleanup easier
 * @param [out] poolPtr    The resulting pool
 *
 * @return a success or error code
 **/
int makeObjectPool(void *entryData, ObjectPool **poolPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy an object pool.
 *
 * @param poolPtr  The pointer holding the pool, which will be nulled
 **/
void freeObjectPool(ObjectPool **poolPtr);

/**
 * Get the entry data from an object pool.
 *
 * @param pool  The pool
 *
 * @return entryData  The entry data which was stored at pool construction time
 **/
void *getObjectPoolEntryData(ObjectPool *pool);

/**
 * Add an entry to an object pool (used at construction time).
 *
 * @param pool   The pool receiving the entry
 * @param entry  The entry to add
 **/
void addEntryToObjectPool(ObjectPool *pool, RingNode *entry);

/**
 * Remove an entry from an object pool (used at destruction time).
 *
 * @param pool  The pool losing an entry
 *
 * @return An entry which will be removed from the pool or NULL if the pool
 *         is empty
 **/
RingNode *removeEntryFromObjectPool(ObjectPool *pool);

/**
 * Acquire an entry from an object pool (asynchronous).
 *
 * @param pool    The pool
 * @param waiter  The entry requestor
 *
 * @return VDO_SUCCESS or an error
 **/
int acquireEntryFromObjectPool(ObjectPool *pool, Waiter *waiter);

/**
 * Return an entry to an object pool.
 *
 * @param pool   The pool
 * @param entry  The entry being returned
 **/
void returnEntryToObjectPool(ObjectPool *pool, RingNode *entry);

/**
 * Suspend an object pool. Once suspended, any requests for objects from the
 * pool will block until the pool is resumed.
 *
 * @param pool        The pool
 * @param completion  The object to notify once all objects have been returned
 *                    to the pool
 **/
void suspendObjectPool(ObjectPool *pool, VDOCompletion *completion);

/**
 * Resume an object pool. If it has been suspended, any requests for objects
 * which were blocked due to suspension of the pool will be honored if there
 * are objects available.
 *
 * @param pool  The pool
 **/
void resumeObjectPool(ObjectPool *pool);

/**
 * Close an object pool. This method will complete the supplied completion
 * whenever the pool has no outstanding entries and no waiters. It does not
 * enforce continued closure of the pool, and may starve.
 *
 * @param pool        The pool
 * @param completion  The object to notify once all objects have been returned
 *                    to the pool and the pool has no waiters
 **/
void closeObjectPool(ObjectPool *pool, VDOCompletion *completion);

/**
 * Open an object pool which has been closed.
 *
 * @param pool  The pool to open
 **/
void openObjectPool(ObjectPool *pool);

/**
 * Return the outage count of an object pool.
 *
 * @param pool  The pool
 *
 * @return the number of times an acquisition request had to wait
 **/
uint64_t getObjectPoolOutageCount(ObjectPool *pool)
  __attribute__((warn_unused_result));

#endif // OBJECT_POOL_H
