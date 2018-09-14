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
 * $Id: //eng/vdo-releases/magnesium-rhel7.6/src/c++/vdo/kernel/instanceNumber.c#1 $
 */

#include "instanceNumber.h"

#include <linux/bitops.h>
#include <linux/mutex.h>

#include "memoryAlloc.h"
#include "numUtils.h"
#include "permassert.h"

/*
 * Track in-use instance numbers using a flat bit array.
 *
 * O(n) run time isn't ideal, but if we have 1000 VDO devices in use
 * simultaneously we still only need to scan 16 words, so it's not
 * likely to be a big deal compared to other resource usage.
 */

enum {
  /**
   * This minimum size for the bit array creates a numbering space of 0-999,
   * which allows successive starts of the same volume to have different
   * instance numbers in any reasonably-sized test. Changing instances on
   * restart allows vdoMonReport to detect that the ephemeral stats have reset
   * to zero.
   **/
  BIT_COUNT_MINIMUM   = 1000,
  /** Grow the bit array by this many bits when needed */
  BIT_COUNT_INCREMENT = 100,
};

static struct mutex   instanceNumberLock;
static unsigned int   bitCount;
static unsigned long *words;
static unsigned int   instanceCount;
static unsigned int   nextInstance;

/**
 * Return the number of bytes needed to store a bit array of the specified
 * capacity in an array of unsigned longs.
 *
 * @param bitCount  The number of bits the array must hold
 *
 * @return the number of bytes needed for the array reperesentation
 **/
static size_t getBitArraySize(unsigned int bitCount)
{
  // Round up to a multiple of the word size and convert to a byte count.
  return (computeBucketCount(bitCount, BITS_PER_LONG) * sizeof(unsigned long));
}

/**
 * Re-allocate the bitmap word array so there will more instance numbers that
 * can be allocated. Since the array is initially NULL, this also initializes
 * the array the first time we allocate an instance number.
 *
 * @return UDS_SUCCESS or an error code from the allocation
 **/
static int growBitArray(void)
{
  unsigned int newCount = maxUInt(bitCount + BIT_COUNT_INCREMENT,
                                  BIT_COUNT_MINIMUM);
  unsigned long *newWords;
  int result = reallocateMemory(words,
                                getBitArraySize(bitCount),
                                getBitArraySize(newCount),
                                "instance number bit array",
                                &newWords);
  if (result != UDS_SUCCESS) {
    return result;
  }

  bitCount = newCount;
  words    = newWords;
  return UDS_SUCCESS;
}

/**********************************************************************/
static int allocateKVDOInstanceLocked(unsigned int *instancePtr)
{
  // If there are no unallocated instances, grow the bit array.
  if (instanceCount >= bitCount) {
    int result = growBitArray();
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  // There must be a zero bit somewhere now. Find it, starting just after the
  // last instance allocated.
  unsigned int instance = find_next_zero_bit(words, bitCount, nextInstance);
  if (instance >= bitCount) {
    // Nothing free after nextInstance, so wrap around to instance zero.
    instance = find_first_zero_bit(words, bitCount);
    int result = ASSERT(instance < bitCount, "impossibly, no zero bit found");
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  __set_bit(instance, words);
  instanceCount += 1;
  nextInstance = instance + 1;
  *instancePtr = instance;
  return UDS_SUCCESS;
}

/**********************************************************************/
int allocateKVDOInstance(unsigned int *instancePtr)
{
  mutex_lock(&instanceNumberLock);
  int result = allocateKVDOInstanceLocked(instancePtr);
  mutex_unlock(&instanceNumberLock);
  return result;
}

/**********************************************************************/
void releaseKVDOInstance(unsigned int instance)
{
  mutex_lock(&instanceNumberLock);
  if (instance >= bitCount) {
    ASSERT_LOG_ONLY(false, "instance number %u must be less than bit count %u",
                    instance, bitCount);
  } else if (test_bit(instance, words) == 0) {
    ASSERT_LOG_ONLY(false, "instance number %u must be allocated", instance);
  } else {
    __clear_bit(instance, words);
    instanceCount -= 1;
  }
  mutex_unlock(&instanceNumberLock);
}

/**********************************************************************/
void initializeInstanceNumberTracking(void)
{
  mutex_init(&instanceNumberLock);
}

/**********************************************************************/
void cleanUpInstanceNumberTracking(void)
{
  ASSERT_LOG_ONLY(instanceCount == 0,
                  "should have no instance numbers still in use, but have %u",
                  instanceCount);
  FREE(words);
  words = NULL;
  bitCount = 0;
  instanceCount = 0;
  nextInstance = 0;
  mutex_destroy(&instanceNumberLock);
}
