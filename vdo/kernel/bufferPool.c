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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bufferPool.c#3 $
 */

#include "bufferPool.h"

#include <linux/delay.h>
#include <linux/sort.h>

#include "logger.h"
#include "memoryAlloc.h"

#include "statusCodes.h"

/*
 * For list nodes on the free-object list, the data field describes
 * the object available for reuse.
 *
 * For nodes on the "spare" list, the data field is meaningless;
 * they're just nodes available for use when we need to add an object
 * pointer to the freeObjectList.
 *
 * These are both "free lists", in a sense; don't get confused!
 */
struct buffer_element {
  struct list_head  list;       // links in current list
  void             *data;       // element data, if on free list
};

struct bufferPool {
  const char              *name; // Pool name
  void                    *data; // Associated pool data
  spinlock_t               lock; // Locks this object
  unsigned int             size; // Total number of buffers
  struct list_head         free_object_list; // List of free buffers
  struct list_head         spare_list_nodes; // Unused list nodes
  unsigned int             num_busy; // Number of buffers in use
  unsigned int             max_busy; // Maximum value of the above
  BufferAllocateFunction  *alloc; // Allocate function for buffer data
  BufferFreeFunction      *free; // Free function for buffer data
  BufferDumpFunction      *dump; // Dump function for buffer data
  struct buffer_element   *bhead; // Array of buffer_element structures
  void                   **objects;
};

/*************************************************************************/
int make_buffer_pool(const char            *pool_name,
                   unsigned int             size,
                   BufferAllocateFunction  *allocate_function,
                   BufferFreeFunction      *free_function,
                   BufferDumpFunction      *dump_function,
                   void                    *pool_data,
                   BufferPool             **pool_ptr)
{
  BufferPool *pool;

  int result = ALLOCATE(1, BufferPool, "buffer pool", &pool);
  if (result != VDO_SUCCESS) {
    logError("buffer pool allocation failure %d", result);
    return result;
  }

  result = ALLOCATE(size, struct buffer_element, "buffer pool elements",
                    &pool->bhead);
  if (result != VDO_SUCCESS) {
    logError("buffer element array allocation failure %d", result);
    free_buffer_pool(&pool);
    return result;
  }

  result = ALLOCATE(size, void *, "object pointers", &pool->objects);
  if (result != VDO_SUCCESS) {
    logError("buffer object array allocation failure %d", result);
    free_buffer_pool(&pool);
    return result;
  }

  pool->name  = pool_name;
  pool->alloc = allocate_function;
  pool->free  = free_function;
  pool->dump  = dump_function;
  pool->data  = pool_data;
  pool->size  = size;
  spin_lock_init(&pool->lock);
  INIT_LIST_HEAD(&pool->free_object_list);
  INIT_LIST_HEAD(&pool->spare_list_nodes);
  struct buffer_element *bh = pool->bhead;
  for (int i = 0; i < pool->size; i++) {
    result = pool->alloc(pool->data, &bh->data);
    if (result != VDO_SUCCESS) {
      logError("verify buffer data allocation failure %d", result);
      free_buffer_pool(&pool);
      return result;
    }
    pool->objects[i] = bh->data;
    list_add(&bh->list, &pool->free_object_list);
    bh++;
  }
  pool->num_busy = pool->max_busy = 0;

  *pool_ptr = pool;
  return VDO_SUCCESS;
}

/*************************************************************************/
void free_buffer_pool(BufferPool **pool_ptr)
{
  BufferPool *pool = *pool_ptr;
  if (pool == NULL) {
    return;
  }

  ASSERT_LOG_ONLY((pool->num_busy == 0),
                  "freeing busy buffer pool, num_busy=%d",
                  pool->num_busy);
  if (pool->objects != NULL) {
    for (int i = 0; i < pool->size; i++) {
      if (pool->objects[i] != NULL) {
        pool->free(pool->data, pool->objects[i]);
      }
    }
    FREE(pool->objects);
  }
  FREE(pool->bhead);
  FREE(pool);
  *pool_ptr = NULL;
}

/*************************************************************************/
static bool inFreeList(BufferPool *pool, void *data)
{
  struct list_head *node;
  list_for_each(node, &pool->free_object_list) {
    if (container_of(node, struct buffer_element, list)->data == data) {
      return true;
    }
  }
  return false;
}

/*************************************************************************/
void dump_buffer_pool(BufferPool *pool, bool dump_elements)
{
  // In order that syslog can empty its buffer, sleep after 35 elements for
  // 4ms (till the second clock tick).  These numbers chosen in October
  // 2012 running on an lfarm.
  enum { ELEMENTS_PER_BATCH = 35 };
  enum { SLEEP_FOR_SYSLOG = 4 };

  if (pool == NULL) {
    return;
  }
  spin_lock(&pool->lock);
  logInfo("%s: %u of %u busy (max %u)", pool->name, pool->num_busy, pool->size,
          pool->max_busy);
  if (dump_elements && (pool->dump != NULL)) {
    int dumped = 0;
    for (int i = 0; i < pool->size; i++) {
      if (!inFreeList(pool, pool->objects[i])) {
        pool->dump(pool->data, pool->objects[i]);
        if (++dumped >= ELEMENTS_PER_BATCH) {
          spin_unlock(&pool->lock);
          dumped = 0;
          msleep(SLEEP_FOR_SYSLOG);
          spin_lock(&pool->lock);
        }
      }
    }
  }
  spin_unlock(&pool->lock);
}

/*************************************************************************/
int alloc_buffer_from_pool(BufferPool *pool, void **dataPtr)
{
  if (pool == NULL) {
    return UDS_INVALID_ARGUMENT;
  }

  spin_lock(&pool->lock);
  if (unlikely(list_empty(&pool->free_object_list))) {
    spin_unlock(&pool->lock);
    logDebug("no free buffers");
    return -ENOMEM;
  }

  struct buffer_element *bh = list_first_entry(&pool->free_object_list,
                                               struct buffer_element,
                                               list);
  list_move(&bh->list, &pool->spare_list_nodes);
  pool->num_busy++;
  if (pool->num_busy > pool->max_busy) {
    pool->max_busy = pool->num_busy;
  }
  *dataPtr = bh->data;
  spin_unlock(&pool->lock);
  return VDO_SUCCESS;

}

/*************************************************************************/
static bool freeBufferToPoolInternal(BufferPool *pool, void *data)
{
  if (unlikely(list_empty(&pool->spare_list_nodes))) {
    return false;
  }
  struct buffer_element *bh = list_first_entry(&pool->spare_list_nodes,
                                               struct buffer_element,
                                               list);
  list_move(&bh->list, &pool->free_object_list);
  bh->data = data;
  pool->num_busy--;
  return true;
}

/*************************************************************************/
void free_buffer_to_pool(BufferPool *pool, void *data)
{
  spin_lock(&pool->lock);
  bool success = freeBufferToPoolInternal(pool, data);
  spin_unlock(&pool->lock);
  if (!success) {
    logDebug("trying to add to free list when already full");
  }
}

/*************************************************************************/
void free_buffers_to_pool(BufferPool *pool, void **data, int count)
{
  spin_lock(&pool->lock);
  bool success = true;
  for (int i = 0; (i < count) && success; i++) {
    success = freeBufferToPoolInternal(pool, data[i]);
  }
  spin_unlock(&pool->lock);
  if (!success) {
    logDebug("trying to add to free list when already full");
  }
}
