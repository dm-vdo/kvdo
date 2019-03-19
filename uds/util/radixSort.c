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
 * $Id: //eng/uds-releases/gloria/src/uds/util/radixSort.c#1 $
 */

/*
 * Radix sort is implemented using an American Flag sort, an unstable,
 * in-place 8-bit radix exchange sort.
 *
 * Adapted from the algorithm in the paper by Peter M. McIlroy, Keith Bostic,
 * and M. Douglas McIlroy, "Engineering Radix Sort".
 * http://www.usenix.org/publications/compsystems/1993/win_mcilroy.pdf
 */

#include "radixSort.h"

#include "compiler.h"
#include "memoryAlloc.h"
#include "stringUtils.h"
#include "typeDefs.h"
#include "uds.h"

enum {
  // Piles smaller than this are handled with a simple insertion sort.
  INSERTION_SORT_THRESHOLD  = 12
};

// Sort keys are pointers to immutable fixed-length arrays of bytes.
typedef const uint8_t * Key;

/**
 * The keys are separated into piles based on the byte in each
 * keys at the current offset, so the number of keys with each
 * byte must be counted.
 **/
typedef struct {
  uint16_t used;       // number of non-empty bins
  uint16_t first;      // index (key byte)  of the first non-empty bin
  uint16_t last;       // index (key byte) of the last non-empty bin
  uint32_t size[256];  // size[byte] == # of occurrences of byte
} Histogram;

/**
 * Sub-tasks are manually managed on a stack, both for performance
 * and to put a logarithmic bound on the stack space needed.
 **/
typedef struct {
  Key      *firstKey;  // Pointers to first and last keys to sort, inclusive.
  Key      *lastKey;
  uint16_t  offset;    // The offset into the key at which to continue sorting.
  uint16_t  length;    // The number of bytes remaining in the sort keys.
} Task;

struct radixSorter {
  unsigned int  count;
  Histogram     bins;
  Key          *pile[256];
  Task         *endOfStack;
  Task          isList[256];
  Task          stack[];
};

/**
 * Compare a segment of two fixed-length keys starting an offset.
 *
 * @param key1    the first key
 * @param key2    the second key
 * @param offset  the offset into the keys of the first byte to compare
 * @param length  the number of bytes remaining in each key
 **/
static INLINE int compare(Key key1, Key key2, uint16_t offset, uint16_t length)
{
  return memcmp(&key1[offset], &key2[offset], length);
}

/**
 * Insert the next unsorted key into an array of sorted keys.
 *
 * @param task  the description of the keys being sorted
 * @param next  the pointer to the unsorted key to insert into
 *              the array of sorted key pointers preceding it
 **/
static INLINE void insertKey(const Task task, Key *next)
{
  // Pull the unsorted key out, freeing up the array slot.
  Key unsorted = *next;
  // Compare the key to the preceding sorted entries, shifting
  // down the ones that are larger.
  while ((--next >= task.firstKey)
         && (compare(unsorted, next[0], task.offset, task.length) < 0)) {
    next[1] = next[0];
  }
  // Insert the key into the last slot that was cleared, sorting it.
  next[1] = unsorted;
}

/**
 * Sort a range of key segments using an insertion sort. This simple sort is
 * faster than the 256-way radix sort when the number of keys to sort is
 * small.
 *
 * @param task  the description of the keys to sort
 **/
static INLINE void insertionSort(const Task task)
{
  // (firstKey .. firstKey) is trivially sorted. Repeatedly insert the next
  // key into the sorted list of keys preceding it, and voila!
  for (Key *next = task.firstKey + 1; next <= task.lastKey; next++) {
    insertKey(task, next);
  }
}

/**
 * Push a sorting task onto the task stack, increasing the stack pointer.
 **/
static INLINE void pushTask(Task    **stackPointer,
                            Key      *firstKey,
                            uint32_t  count,
                            uint16_t  offset,
                            uint16_t  length)
{
  Task *task = (*stackPointer)++;
  task->firstKey = firstKey;
  task->lastKey  = &firstKey[count - 1];
  task->offset   = offset;
  task->length   = length;
}

/**********************************************************************/
static INLINE void swapKeys(Key *a, Key *b)
{
  Key c = *a;
  *a = *b;
  *b = c;
}

/**
 * Count the number of times each byte value appears in in the arrays of keys
 * to sort at the current offset, keeping track of the number of non-empty
 * bins, and the index of the first and last non-empty bin.
 *
 * @param task  the description of the keys to sort
 * @param bins  the histogram bins receiving the counts
 **/
static INLINE void measureBins(const Task task, Histogram *bins)
{
  // Set bogus values that will will be replaced by min and max, respectively.
  bins->first = UINT8_MAX;
  bins->last = 0;

  // Subtle invariant: bins->used and bins->size[] are zero because the
  // sorting code clears it all out as it goes. Even though this structure is
  // re-used, we don't need to pay to zero it before starting a new tally.

  for (Key *keyPtr = task.firstKey; keyPtr <= task.lastKey; keyPtr++) {
    // Increment the count for the byte in the key at the current offset.
    uint8_t bin = (*keyPtr)[task.offset];
    uint32_t size = ++bins->size[bin];

    // Track non-empty bins when the count transitions from zero to one.
    if (size == 1) {
      bins->used += 1;
      if (bin < bins->first) {
        bins->first = bin;
      }
      if (bin > bins->last) {
        bins->last = bin;
      }
    }
  }
}

/**
 * Convert the bin sizes to pointers to where each pile goes.
 *
 *   pile[0] = firstKey + bin->size[0],
 *   pile[1] = pile[0]  + bin->size[1], etc.
 *
 * After the keys are moved to the appropriate pile, we'll need to sort
 * each of the piles by the next radix position.  A new task is put on the
 * stack for each pile containing lots of keys, or a new task is is put on
 * the list for each pile containing few keys.
 *
 * @param stack      pointer the top of the stack
 * @param endOfStack the end of the stack
 * @param list       pointer the head of the list
 * @param pile       array that will be filled pointers to the end of each pile
 * @param bins       the histogram of the sizes of each pile
 * @param firstKey   the first key of the stack
 * @param offset     the next radix position to sort by
 * @param length     the number of bytes remaining in the sort keys
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int pushBins(Task       **stack,
                           Task       *endOfStack,
                           Task       **list,
                           Key        *pile[],
                           Histogram  *bins,
                           Key        *firstKey,
                           uint16_t    offset,
                           uint16_t    length)
{
  Key *pileStart = firstKey;
  for (int bin = bins->first; ; bin++) {
    uint32_t size = bins->size[bin];
    // Skip empty piles.
    if (size == 0) {
      continue;
    }
    // There's no need to sort empty keys.
    if (length > 0) {
      if (size > INSERTION_SORT_THRESHOLD) {
        if (*stack >= endOfStack) {
          return UDS_BAD_STATE;
        }
        pushTask(stack, pileStart, size, offset, length);
      } else if (size > 1) {
        pushTask(list, pileStart, size, offset, length);
      }
    }
    pileStart += size;
    pile[bin] = pileStart;
    if (--bins->used == 0) {
      break;
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int makeRadixSorter(unsigned int count, RadixSorter **sorter)
{
  unsigned int stackSize = count / INSERTION_SORT_THRESHOLD;
  RadixSorter *radixSorter;
  int result = ALLOCATE_EXTENDED(RadixSorter, stackSize, Task, __func__,
                                 &radixSorter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  radixSorter->count = count;
  radixSorter->endOfStack = radixSorter->stack + stackSize;
  *sorter = radixSorter;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeRadixSorter(RadixSorter *sorter)
{
  FREE(sorter);
}

/**********************************************************************/
int radixSort(RadixSorter         *sorter,
              const unsigned char *keys[],
              unsigned int         count,
              unsigned short       length)
{
  // All zero-length keys are identical and therefore already sorted.
  if ((count == 0) || (length == 0)) {
    return UDS_SUCCESS;
  }

  // The initial task is to sort the entire length of all the keys.
  Task start = {
    .firstKey = keys,
    .lastKey  = &keys[count - 1],
    .offset   = 0,
    .length   = length,
  };

  if (count <= INSERTION_SORT_THRESHOLD) {
    insertionSort(start);
    return UDS_SUCCESS;
  }

  if (count > sorter->count) {
    return UDS_INVALID_ARGUMENT;
  }

  Histogram  *bins  = &sorter->bins;
  Key       **pile  = sorter->pile;
  Task       *sp    = sorter->stack;

  /*
   * Repeatedly consume a sorting task from the stack and process it, pushing
   * new sub-tasks onto to the stack for each radix-sorted pile. When all
   * tasks and sub-tasks have been processed, the stack will be empty and all
   * the keys in the starting task will be fully sorted.
   */
  for (*sp = start; sp >= sorter->stack; sp--) {
    const Task task = *sp;
    measureBins(task, bins);

    // Now that we know how large each bin is, generate pointers for each of
    // the piles and push a new task to sort each pile by the next radix byte.
    Task *lp = sorter->isList;
    int result = pushBins(&sp, sorter->endOfStack, &lp, pile, bins,
                          task.firstKey, task.offset + 1, task.length - 1);
    if (result != UDS_SUCCESS) {
      memset(bins, 0, sizeof(*bins));
      return result;
    }
    // Now bins->used is zero again.

    // Don't bother processing the last pile--when piles 0..N-1 are all in
    // place, then pile N must also be in place.
    Key *end = task.lastKey - bins->size[bins->last];
    bins->size[bins->last] = 0;

    for (Key *fence = task.firstKey; fence <= end; ) {
      uint8_t bin;
      Key key = *fence;
      // The radix byte of the key tells us which pile it belongs in. Swap it
      // for an unprocessed item just below that pile, and repeat.
      while (--pile[bin = key[task.offset]] > fence) {
        swapKeys(pile[bin], &key);
      }
      // The pile reached the fence. Put the key at the bottom of that pile.
      // completing it, and advance the fence to the next pile.
      *fence = key;
      fence += bins->size[bin];
      bins->size[bin] = 0;
    }
    // Now bins->size[] is all zero again.

    // When the number of keys in a task gets small enough, its faster to use
    // an insertion sort than to keep subdividing into tiny piles.
    while (--lp >= sorter->isList) {
      insertionSort(*lp);
    }
  }
  return UDS_SUCCESS;
}
