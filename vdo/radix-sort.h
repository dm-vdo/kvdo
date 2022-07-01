/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RADIX_SORT_H
#define RADIX_SORT_H

#include "compiler.h"

/*
 * The implementation uses one large object allocated on the heap.  This
 * large object can be reused as many times as desired.  There is no
 * further heap usage by the sorting.
 */
struct radix_sorter;

/**
 * Reserve the heap storage needed by the radix_sort routine.  The amount of
 * heap space is logarithmically proportional to the number of keys.
 *
 * @param count   The maximum number of keys to be sorted
 * @param sorter  The struct radix_sorter object is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
make_radix_sorter(unsigned int count, struct radix_sorter **sorter);

/**
 * Free the heap storage needed by the radix_sort routine.
 *
 * @param sorter  The struct radix_sorter object to free
 **/
void free_radix_sorter(struct radix_sorter *sorter);

/**
 * Sort pointers to fixed-length keys (arrays of bytes) using a radix sort.
 *
 * The sort implementation is unstable--relative ordering of equal keys is not
 * preserved. The implementation does not use any heap allocation.
 *
 * @param [in] sorter  the heap storage used by the sorting
 * @param      keys    the array of key pointers to sort (modified in place)
 * @param [in] count   the number of keys
 * @param [in] length  the length of every key, in bytes
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check radix_sort(struct radix_sorter *sorter,
			    const unsigned char *keys[],
			    unsigned int count,
			    unsigned short length);

#endif /* RADIX_SORT_H */
