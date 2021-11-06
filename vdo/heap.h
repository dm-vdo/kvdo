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

#ifndef HEAP_H
#define HEAP_H

#include "typeDefs.h"

/**
 * Prototype for functions which compare two array elements. All the time
 * complexity claims in this module assume this operation has O(1) time
 * complexity.
 *
 * @param item1  The first element to compare
 * @param item2  The second element to compare
 *
 * @return An integer which is less than, equal to, or greater than 0
 *         depending on whether item1 is less than, equal to, or greater
 *         than item2, respectively
 **/
typedef int heap_comparator(const void *item1, const void *item2);

/**
 * Prototype for functions which swap two array elements.
 *
 * @param item1  The first element to swap
 * @param item2  The second element to swap
 **/
typedef void heap_swapper(void *item1, void *item2);

/**
 * A heap array can be any array of fixed-length elements in which the heap
 * invariant can be established. In a max-heap, every child of a node must be
 * at least as large as its children. Once that invariant is established in an
 * array by calling build_heap(), all the other heap operations may be used on
 * that array.
 **/
struct heap {
	/** the 1-based array of heap elements (nodes) */
	byte *array;
	/** the function to use to compare two elements */
	heap_comparator *comparator;
	/** the function to use to swap two elements */
	heap_swapper *swapper;
	/** the maximum number of elements that can be stored */
	size_t capacity;
	/** the size of every element (in bytes) */
	size_t element_size;
	/** the current number of elements in the heap */
	size_t count;
};

void initialize_heap(struct heap *heap, heap_comparator *comparator,
		     heap_swapper *swapper, void *array, size_t capacity,
		     size_t element_size);

void build_heap(struct heap *heap, size_t count);

/**
 * Check whether the heap is currently empty.
 *
 * @param heap  The heap to query
 *
 * @return <code>true</code> if there are no elements in the heap
 **/
static inline bool is_heap_empty(const struct heap *heap)
{
	return (heap->count == 0);
}

bool pop_max_heap_element(struct heap *heap, void *element_ptr);

size_t sort_heap(struct heap *heap);

void *sort_next_heap_element(struct heap *heap);

#endif /* HEAP_H */
