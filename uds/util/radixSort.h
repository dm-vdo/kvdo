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
 * $Id: //eng/uds-releases/gloria/src/uds/util/radixSort.h#1 $
 */

#ifndef RADIX_SORT_H
#define RADIX_SORT_H

/*
 * The implementation uses one large object allocated on the heap.  This
 * large object can be reused as many times as desired.  There is no
 * further heap usage by the sorting.
 */
typedef struct radixSorter RadixSorter;

/**
 * Reserve the heap storage needed by the radixSort routine.  The amount of
 * heap space is logarithmically proportional to the number of keys.
 *
 * @param count   The maximum number of keys to be sorted
 * @param sorter  The RadixSorter object is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeRadixSorter(unsigned int count, RadixSorter **sorter)
  __attribute__((warn_unused_result));

/**
 * Free the heap storage needed by the radixSort routine.
 *
 * @param sorter  The RadixSorter object to free
 **/
void freeRadixSorter(RadixSorter *sorter);

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
int radixSort(RadixSorter         *sorter,
              const unsigned char *keys[],
              unsigned int         count,
              unsigned short       length)
  __attribute__((warn_unused_result));

#endif /* RADIX_SORT_H */
