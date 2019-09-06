/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vioPool.h#4 $
 */

#ifndef VIO_POOL_H
#define VIO_POOL_H

#include "permassert.h"

#include "completion.h"
#include "types.h"
#include "waitQueue.h"

/**
 * A VIOPool is a collection of preallocated VIOs used to write arbitrary
 * metadata blocks.
 **/

/**
 * A vio_pool_entry is the pair of VIO and buffer whether in use or not.
 **/
struct vio_pool_entry {
  RingNode  node;
  VIO      *vio;
  void     *buffer;
  void     *parent;
  void     *context;
};

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
 * @param [in]  threadID        the ID of the thread using this pool
 * @param [in]  vioConstructor  the constructor for VIOs in the pool
 * @param [in]  context         the context that each entry will have
 * @param [out] poolPtr         the resulting pool
 *
 * @return a success or error code
 **/
int makeVIOPool(PhysicalLayer   *layer,
                size_t           poolSize,
                ThreadID         threadID,
                VIOConstructor  *vioConstructor,
                void            *context,
                VIOPool        **poolPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy a VIO pool
 *
 * @param poolPtr  the pointer holding the pool, which will be nulled out
 **/
void freeVIOPool(VIOPool **poolPtr);

/**
 * Check whether an VIO pool has outstanding entries.
 *
 * @return <code>true</code> if the pool is busy
 **/
bool isVIOPoolBusy(VIOPool *pool)
  __attribute__((warn_unused_result));

/**
 * Acquire a VIO and buffer from the pool (asynchronous).
 *
 * @param pool    the VIO pool
 * @param waiter  object that is requesting a VIO
 *
 * @return VDO_SUCCESS or an error
 **/
int acquireVIOFromPool(VIOPool *pool, Waiter *waiter);

/**
 * Return a VIO and its buffer to the pool.
 *
 * @param pool   the VIO pool
 * @param entry  a VIO pool entry
 **/
void returnVIOToPool(VIOPool *pool, struct vio_pool_entry *entry);

/**
 * Convert a RingNode to the vio_pool_entry that contains it.
 *
 * @param node  The RingNode to convert
 *
 * @return The vio_pool_entry wrapping the RingNode
 **/
static inline struct vio_pool_entry *asVIOPoolEntry(RingNode *node)
{
  STATIC_ASSERT(offsetof(struct vio_pool_entry, node) == 0);
  return (struct vio_pool_entry *) node;
}

/**
 * Return the outage count of an VIO pool.
 *
 * @param pool  The pool
 *
 * @return the number of times an acquisition request had to wait
 **/
uint64_t getVIOPoolOutageCount(VIOPool *pool)
  __attribute__((warn_unused_result));

#endif // VIO_POOL_H
