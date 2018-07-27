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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vioPool.h#2 $
 */

#ifndef VIO_POOL_H
#define VIO_POOL_H

#include "permassert.h"

#include "completion.h"
#include "objectPool.h"
#include "types.h"
#include "waitQueue.h"

/**
 * An VIOPool is a collection of preallocated VIOs used to write
 * arbitrary metadata blocks.
 **/

/**
 * An VIOPoolEntry is the pair of VIO and buffer whether in use or not.
 **/
typedef struct {
  RingNode  node;
  VIO      *vio;
  void     *buffer;
  void     *parent;
  void     *context;
} VIOPoolEntry;

/**
 * A function which constructs a VIO for a pool.
 *
 * @param [in]  layer   The physical layer in which the VIO will operate
 * @param [in]  parent  The parent of the VIO
 * @param [in]  buffer  The data buffer for the VIO
 * @param [out] vioPtr  A pointer to hold the new VIO
 **/
typedef int VIOConstructor(PhysicalLayer  *layer,
                           void           *parent,
                           void           *buffer,
                           VIO           **vioPtr);

/**
 * Create a new VIO pool.
 *
 * @param [in]  layer           the physical layer to write to and read from
 * @param [in]  poolSize        the number of VIOs in the pool
 * @param [in]  vioConstructor  the constructor for VIOs in the pool
 * @param [in]  context         the context that each entry will have
 * @param [out] poolPtr         the resulting pool
 *
 * @return a success or error code
 **/
int makeVIOPool(PhysicalLayer   *layer,
                size_t           poolSize,
                VIOConstructor  *vioConstructor,
                void            *context,
                ObjectPool     **poolPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy a VIO pool
 *
 * @param poolPtr  the pointer holding the pool, which will be nulled out
 **/
void freeVIOPool(ObjectPool **poolPtr);

/**
 * Acquire a VIO and buffer from the pool (asynchronous).
 *
 * @param pool    the VIO pool
 * @param waiter  object that is requesting a VIO
 *
 * @return VDO_SUCCESS or an error
 **/
static inline int acquireVIOFromPool(ObjectPool *pool, Waiter *waiter)
{
  return acquireEntryFromObjectPool(pool, waiter);
}

/**
 * Return a VIO and its buffer to the pool.
 *
 * @param pool   the VIO pool
 * @param entry  a VIO pool entry
 **/
void returnVIOToPool(ObjectPool *pool, VIOPoolEntry *entry);

/**
 * Convert a RingNode to the VIO pool entry that contains it.
 *
 * @param node  The RingNode to convert
 *
 * @return The VIOPoolEntry wrapping the RingNode
 **/
static inline VIOPoolEntry *asVIOPoolEntry(RingNode *node)
{
  STATIC_ASSERT(offsetof(VIOPoolEntry, node) == 0);
  return (VIOPoolEntry *) node;
}

#endif // VIO_POOL_H
