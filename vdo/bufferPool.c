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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bufferPool.c#19 $
 */

#include "bufferPool.h"

#include <linux/delay.h>
#include <linux/sort.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "statusCodes.h"

/*
 * For list nodes on the free-object list, the data field describes
 * the object available for reuse.
 *
 * For nodes on the "spare" list, the data field is meaningless;
 * they're just nodes available for use when we need to add an object
 * pointer to the free_object_list.
 *
 * These are both "free lists", in a sense; don't get confused!
 */
struct buffer_element {
	struct list_head list;	// links in current list
	void *data;		// element data, if on free list
};

struct buffer_pool {
	const char *name; // Pool name
	spinlock_t lock; // Locks this object
	unsigned int size; // Total number of buffers
	struct list_head free_object_list; // List of free buffers
	struct list_head spare_list_nodes; // Unused list nodes
	unsigned int num_busy; // Number of buffers in use
	unsigned int max_busy; // Maximum value of the above
	buffer_allocate_function *alloc; // Allocate function for buffer data
	buffer_free_function *free; // Free function for buffer data
	buffer_dump_function *dump; // Dump function for buffer data
	struct buffer_element *bhead; // Array of buffer_element
	void **objects;
};

/*************************************************************************/
int make_buffer_pool(const char *pool_name,
		     unsigned int size,
		     buffer_allocate_function *allocate_function,
		     buffer_free_function *free_function,
		     buffer_dump_function *dump_function,
		     struct buffer_pool **pool_ptr)
{
	struct buffer_pool *pool;
	struct buffer_element *bh;
	int i;

	int result = UDS_ALLOCATE(1, struct buffer_pool, "buffer pool", &pool);

	if (result != VDO_SUCCESS) {
		uds_log_error("buffer pool allocation failure %d", result);
		return result;
	}

	result = UDS_ALLOCATE(size, struct buffer_element,
			      "buffer pool elements", &pool->bhead);
	if (result != VDO_SUCCESS) {
		uds_log_error("buffer element array allocation failure %d",
			      result);
		free_buffer_pool(pool);
		return result;
	}

	result = UDS_ALLOCATE(size, void *, "object pointers", &pool->objects);
	if (result != VDO_SUCCESS) {
		uds_log_error("buffer object array allocation failure %d",
			      result);
		free_buffer_pool(pool);
		return result;
	}

	pool->name = pool_name;
	pool->alloc = allocate_function;
	pool->free = free_function;
	pool->dump = dump_function;
	pool->size = size;
	spin_lock_init(&pool->lock);
	INIT_LIST_HEAD(&pool->free_object_list);
	INIT_LIST_HEAD(&pool->spare_list_nodes);
	bh = pool->bhead;

	for (i = 0; i < pool->size; i++) {
		result = pool->alloc(&bh->data);
		if (result != VDO_SUCCESS) {
			uds_log_error("verify buffer data allocation failure %d",
				      result);
			free_buffer_pool(pool);
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
void free_buffer_pool(struct buffer_pool *pool)
{
	if (pool == NULL) {
		return;
	}

	ASSERT_LOG_ONLY((pool->num_busy == 0),
			"freeing busy buffer pool, num_busy=%d",
			pool->num_busy);
	if (pool->objects != NULL) {
		int i;

		for (i = 0; i < pool->size; i++) {
			if (pool->objects[i] != NULL) {
				pool->free(UDS_FORGET(pool->objects[i]));
			}
		}
		UDS_FREE(UDS_FORGET(pool->objects));
	}

	UDS_FREE(UDS_FORGET(pool->bhead));
	UDS_FREE(pool);
}

/*************************************************************************/
static bool in_free_list(struct buffer_pool *pool, void *data)
{
	struct list_head *node;
	struct buffer_element *bh;

	list_for_each(node, &pool->free_object_list) {
		bh = list_entry(node, struct buffer_element, list);
		if (bh->data == data) {
			return true;
		}
	}
	return false;
}

/*************************************************************************/
void dump_buffer_pool(struct buffer_pool *pool, bool dump_elements)
{
	// In order that syslog can empty its buffer, sleep after 35 elements
	// for 4ms (till the second clock tick).  These numbers chosen in
	// October 2012 running on an lfarm.
	enum { ELEMENTS_PER_BATCH = 35 };
	enum { SLEEP_FOR_SYSLOG = 4000 };

	if (pool == NULL) {
		return;
	}
	spin_lock(&pool->lock);
	uds_log_info("%s: %u of %u busy (max %u)",
		     pool->name, pool->num_busy, pool->size, pool->max_busy);
	if (dump_elements && (pool->dump != NULL)) {
		int dumped = 0;
		int i;

		for (i = 0; i < pool->size; i++) {
			if (!in_free_list(pool, pool->objects[i])) {
				pool->dump(pool->objects[i]);
				if (++dumped >= ELEMENTS_PER_BATCH) {
					spin_unlock(&pool->lock);
					dumped = 0;
					fsleep(SLEEP_FOR_SYSLOG);
					spin_lock(&pool->lock);
				}
			}
		}
	}
	spin_unlock(&pool->lock);
}

/*************************************************************************/
int alloc_buffer_from_pool(struct buffer_pool *pool, void **data_ptr)
{
	struct buffer_element *bh;

	if (pool == NULL) {
		return UDS_INVALID_ARGUMENT;
	}

	spin_lock(&pool->lock);
	if (unlikely(list_empty(&pool->free_object_list))) {
		spin_unlock(&pool->lock);
		uds_log_debug("no free buffers");
		return -ENOMEM;
	}

	bh = list_first_entry(&pool->free_object_list,
			      struct buffer_element, list);
	list_move(&bh->list, &pool->spare_list_nodes);
	pool->num_busy++;
	if (pool->num_busy > pool->max_busy) {
		pool->max_busy = pool->num_busy;
	}
	*data_ptr = bh->data;
	spin_unlock(&pool->lock);
	return VDO_SUCCESS;
}

/*************************************************************************/
static bool free_buffer_to_pool_internal(struct buffer_pool *pool, void *data)
{
	struct buffer_element *bh;

	if (unlikely(list_empty(&pool->spare_list_nodes))) {
		return false;
	}
	bh = list_first_entry(&pool->spare_list_nodes,
			      struct buffer_element, list);
	list_move(&bh->list, &pool->free_object_list);
	bh->data = data;
	pool->num_busy--;
	return true;
}

/*************************************************************************/
void free_buffer_to_pool(struct buffer_pool *pool, void *data)
{
	bool success;

	spin_lock(&pool->lock);
	success = free_buffer_to_pool_internal(pool, data);

	spin_unlock(&pool->lock);
	if (!success) {
		uds_log_debug("trying to add to free list when already full");
	}
}

/*************************************************************************/
void free_buffers_to_pool(struct buffer_pool *pool, void **data, int count)
{
	bool success = true;
	int i;

	spin_lock(&pool->lock);

	for (i = 0; (i < count) && success; i++) {
		success = free_buffer_to_pool_internal(pool, data[i]);
	}
	spin_unlock(&pool->lock);
	if (!success) {
		uds_log_debug("trying to add to free list when already full");
	}
}
