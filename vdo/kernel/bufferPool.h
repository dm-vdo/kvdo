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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bufferPool.h#3 $
 */
#ifndef BUFFERPOOL_H
#define BUFFERPOOL_H

/*
 * We need bug.h because in 3.10, kernel.h (indirectly) defines
 * ARRAY_SIZE as a macro which (indirectly and conditionally) uses
 * BUILD_BUG_ON_ZERO, which is defined in bug.h, which is *not*
 * included. In earlier versions like 3.2 it Just Worked.
 */
#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/types.h>

typedef struct bufferPool BufferPool;

typedef int BufferAllocateFunction(void *poolData, void **dataPtr);
typedef void BufferFreeFunction(void *poolData, void *data);
typedef void BufferDumpFunction(void *poolData, void *data);

/**
 * Creates a generic pool of buffer data. The elements in the pool are
 * allocated up front and placed on a free list, which manages the
 * reuse of the individual buffers in the pool.
 *
 * @param [in]  pool_name          Name of the pool
 * @param [in]  size               The number of elements to create for this
 *                                 pool
 * @param [in]  allocate_function  The function to call to create the actual
 *                                 data for each element
 * @param [in]  free_function      The function to call to free the actual
 *                                 data for each element
 * @param [in]  dump_function      The function to call to dump the actual
 *                                 data for each element into the log
 * @param [in]  pool_data          A pointer to the pool's associated data
 * @param [out] pool_ptr           A pointer to hold the pool that was created
 *
 * @return a success or error code
 */
int make_buffer_pool(const char              *pool_name,
                     unsigned int             size,
                     BufferAllocateFunction  *allocate_function,
                     BufferFreeFunction      *free_function,
                     BufferDumpFunction      *dump_function,
                     void                    *pool_data,
                     BufferPool             **pool_ptr)
  __attribute__((warn_unused_result));

/**
 * Free a buffer pool and null out the reference to it. This will free
 * all the elements of the pool as well.
 *
 * @param [in]  pool_ptr   The reference to the pool to free
 **/
void free_buffer_pool(BufferPool **pool_ptr);

/**
 * Dump a buffer pool to the log.
 *
 * @param [in] pool           The buffer pool to allocate from
 * @param [in] dump_elements  True for complete output, or false for a
 *                            one-line summary
 **/
void dump_buffer_pool(BufferPool *pool, bool dump_elements);

/**
 * Acquires a free buffer from the free list of the pool and
 * returns it's associated data.
 *
 * @param [in]  pool      The buffer pool to allocate from
 * @param [out] data_ptr   A pointer to hold the buffer data
 *
 * @return a success or error code
 */
int alloc_buffer_from_pool(BufferPool *pool, void **data_ptr)
  __attribute__((warn_unused_result));

/**
 * Returns a buffer to the free list of a pool
 *
 * @param [in] pool   The buffer pool to return the buffer to
 * @param [in] data   The buffer data to return
 */
void free_buffer_to_pool(BufferPool *pool, void *data);

/**
 * Returns a set of buffers to the free list of a pool
 *
 * @param [in] pool   The buffer pool to return the buffer to
 * @param [in] data   The buffer data to return
 * @param [in] count  Number of entries in the data array
 */
void free_buffers_to_pool(BufferPool *pool, void **data, int count);

/**
 * Control structure for freeing (releasing back to the pool) pointers
 * in batches.
 *
 * Since the objects stored in a buffer pool are completely opaque,
 * some external data structure is needed to manage a collection of
 * them. This is a simple helper for doing that, since we're freeing
 * batches of objects in a couple different places. Within the pool
 * itself there's a pair of linked lists, but getting at them requires
 * the locking that we're trying to minimize.
 *
 * We collect pointers until the array is full or until there are no
 * more available, and we call free_buffers_to_pool to release a batch
 * all at once.
 **/
struct free_buffer_pointers {
  BufferPool *pool;
  int         index;
  void       *pointers[30]; // size is arbitrary
};

/**
 * Initialize the control structure for batching buffer pointers to be
 * released to their pool.
 *
 * @param [out] fbp   The (caller-allocated) control structure
 * @param [in]  pool  The buffer pool to return objects to.
 **/
static inline void initFreeBufferPointers(struct free_buffer_pointers *fbp,
                                          BufferPool                  *pool)
{
  fbp->index = 0;
  fbp->pool  = pool;
}

/**
 * Release any buffers left in the collection.
 *
 * @param [in]  fbp  The control structure
 **/
static inline void freeBufferPointers(struct free_buffer_pointers *fbp)
{
  free_buffers_to_pool(fbp->pool, fbp->pointers, fbp->index);
  fbp->index = 0;
}

/**
 * Add another buffer pointer to the collection, and if we're full,
 * release the whole batch to the pool.
 *
 * @param [in]  fbp      The control structure
 * @param [in]  pointer  The buffer pointer to release
 **/
static inline void addFreeBufferPointer(struct free_buffer_pointers *fbp,
                                        void                        *pointer)
{
  fbp->pointers[fbp->index] = pointer;
  fbp->index++;
  if (fbp->index == ARRAY_SIZE(fbp->pointers)) {
    freeBufferPointers(fbp);
  }
}

#endif /* BUFFERPOOL_H */
