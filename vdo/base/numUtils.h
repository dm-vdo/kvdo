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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/numUtils.h#1 $
 *
 * THIS FILE IS A CANDIDATE FOR THE EVENTUAL UTILITY LIBRARY.
 */

#ifndef NUM_UTILS_H
#define NUM_UTILS_H

#include "common.h"
#include "numeric.h"

#include "types.h"

/**
 * Return true if and only if a number is a power of two.
 **/
static inline bool isPowerOfTwo(uint64_t n)
{
  return (n > 0) && ((n & (n - 1)) == 0);
}

/**
 * Efficiently calculate the base-2 logarithm of a number truncated to an
 * integer value.
 *
 * This also happens to be the bit index of the highest-order non-zero bit in
 * the binary representation of the number, which can easily be used to
 * calculate the bit shift corresponding to a bit mask or an array capacity,
 * or to calculate the binary floor or ceiling (next lowest or highest power
 * of two).
 *
 * @param n  The input value
 *
 * @return the integer log2 of the value, or -1 if the value is zero
 **/
static inline int logBaseTwo(uint64_t n)
{
  if (n == 0) {
    return -1;
  }
  // Many CPUs, including x86, directly support this calculation, so use the
  // GCC function for counting the number of leading high-order zero bits.
  return 63 - __builtin_clzll(n);
}

/**
 * Find the minimum of two physical block numbers.
 **/
__attribute__((warn_unused_result))
static inline PhysicalBlockNumber minBlock(PhysicalBlockNumber a,
                                           PhysicalBlockNumber b)
{
  return (a < b) ? a : b;
}

/**
 * Find the maximum of two physical block numbers.
 **/
__attribute__((warn_unused_result))
static inline PhysicalBlockNumber maxBlock(PhysicalBlockNumber a,
                                           PhysicalBlockNumber b)
{
  return (a > b) ? a : b;
}

/**
 * Find the minimum of two block counts.
 **/
__attribute__((warn_unused_result))
static inline BlockCount minBlockCount(BlockCount a, BlockCount b)
{
  return (a < b) ? a : b;
}

/**
 * Find the maximum of two block counts.
 **/
__attribute__((warn_unused_result))
static inline BlockCount maxBlockCount(BlockCount a, BlockCount b)
{
  return (a > b) ? a : b;
}

/**
 * Find the minimum of two sequence numbers.
 **/
__attribute__((warn_unused_result))
static inline SequenceNumber minSequenceNumber(SequenceNumber a,
                                               SequenceNumber b)
{
  return (a < b) ? a : b;
}

/**
 * Return the minimum of two page counts.
 **/
__attribute__((warn_unused_result))
static inline PageCount minPageCount(PageCount a, PageCount b)
{
  return (a < b) ? a : b;
}

/**
 * Return the maximum of two page counts.
 **/
__attribute__((warn_unused_result))
static inline PageCount maxPageCount(PageCount a, PageCount b)
{
  return (a > b) ? a : b;
}

/**
 * Round upward towards the nearest multiple of quantum.
 *
 * @param number        a number
 * @param quantum       the quantum
 *
 * @return the least multiple of quantum not less than number
 **/
__attribute__((warn_unused_result))
static inline size_t roundUpToMultipleSizeT(size_t number, size_t quantum)
{
  return number + quantum - 1 - ((number + quantum - 1) % quantum);
}

/**
 * Round upward towards the nearest multiple of quantum for uint64_t
 *
 * @param number        a number
 * @param quantum       the quantum
 *
 * @return the least multiple of quantum not less than number
 **/
__attribute__((warn_unused_result))
static inline uint64_t roundUpToMultipleUInt64T(uint64_t number,
                                                uint64_t quantum)
{
  return number + quantum - 1 - ((number + quantum - 1) % quantum);
}

/**
 * Check whether the given value is between the lower and upper bounds,
 * within a cyclic range of values from 0 to (modulus - 1). The value
 * and both bounds must be smaller than the modulus.
 *
 * @param lower    The lowest value to accept
 * @param value    The value to check
 * @param upper    The highest value to accept
 * @param modulus  The size of the cyclic space, no more than 2^15
 *
 * @return <code>true</code> if the value is in range
 **/
static inline bool inCyclicRange(uint16_t lower,
                                 uint16_t value,
                                 uint16_t upper,
                                 uint16_t modulus)
{
  if (value < lower) {
    value += modulus;
  }
  if (upper < lower) {
    upper += modulus;
  }
  return (value <= upper);
}

/**
 * Compute the number of buckets of a given size which are required to hold a
 * given number of objects.
 *
 * @param objectCount  The number of objects to hold
 * @param bucketSize   The size of a bucket
 *
 * @return The number of buckets required
 **/
static inline uint64_t computeBucketCount(uint64_t objectCount,
                                          uint64_t bucketSize)
{
  uint64_t quotient = objectCount / bucketSize;
  if ((objectCount % bucketSize) > 0) {
    ++quotient;
  }
  return quotient;
}

#endif // NUM_UTILS_H
