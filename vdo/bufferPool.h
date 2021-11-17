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
 */
#ifndef BUFFERPOOL_H
#define BUFFERPOOL_H

#include <linux/kernel.h>
#include <linux/types.h>

struct buffer_pool;

typedef int buffer_allocate_function(void **data_ptr);
typedef void buffer_free_function(void *data);
typedef void buffer_dump_function(void *data);

int __must_check make_buffer_pool(const char *pool_name,
				  unsigned int size,
				  buffer_allocate_function *allocate_function,
				  buffer_free_function *free_function,
				  buffer_dump_function *dump_function,
				  struct buffer_pool **pool_ptr);
void free_buffer_pool(struct buffer_pool *pool);
void dump_buffer_pool(struct buffer_pool *pool, bool dump_elements);
int __must_check
alloc_buffer_from_pool(struct buffer_pool *pool, void **data_ptr);
void free_buffer_to_pool(struct buffer_pool *pool, void *data);
void free_buffers_to_pool(struct buffer_pool *pool, void **data, int count);

/**
 * struct free_buffer_pointers - Control structure for freeing to a
 * &struct buffer_pool in batches.
 * @pool: The pool to return the buffers to
 * @index: The number of items in this batch so far
 * @pointers: Pointers to the items to be freed
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
 */
struct free_buffer_pointers {
	struct buffer_pool *pool;
	int index;
	void *pointers[30]; /* size is arbitrary */
};

static inline void init_free_buffer_pointers(struct free_buffer_pointers *fbp,
					     struct buffer_pool *pool)
{
	fbp->index = 0;
	fbp->pool = pool;
}
static inline void free_buffer_pointers(struct free_buffer_pointers *fbp)
{
	free_buffers_to_pool(fbp->pool, fbp->pointers, fbp->index);
	fbp->index = 0;
}

/*
 * Add another buffer pointer to the collection, and if the batch is full,
 * release the whole batch to the pool.
 */
static inline void add_free_buffer_pointer(struct free_buffer_pointers *fbp,
					   void *pointer)
{
	fbp->pointers[fbp->index] = pointer;
	fbp->index++;
	if (fbp->index == ARRAY_SIZE(fbp->pointers)) {
		free_buffer_pointers(fbp);
	}
}

#endif /* BUFFERPOOL_H */
