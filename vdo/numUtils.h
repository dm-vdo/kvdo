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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/numUtils.h#1 $
 *
 * THIS FILE IS A CANDIDATE FOR THE EVENTUAL UTILITY LIBRARY.
 */

#ifndef NUM_UTILS_H
#define NUM_UTILS_H

#include "common.h"
#include "numeric.h"

#include "types.h"

#include <linux/log2.h>

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
static inline int log_base_two(uint64_t n)
{
	if (n == 0) {
		return -1;
	}
	// Many CPUs, including x86, directly support this calculation, so use
	// the GCC function for counting the number of leading high-order zero
	// bits.
	return 63 - __builtin_clzll(n);
}

/**
 * Round upward towards the nearest multiple of quantum.
 *
 * @param number        a number
 * @param quantum       the quantum
 *
 * @return the least multiple of quantum not less than number
 **/
static inline size_t __must_check
round_up_to_multiple_size_t(size_t number, size_t quantum)
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
static inline bool in_cyclic_range(uint16_t lower, uint16_t value,
				   uint16_t upper, uint16_t modulus)
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
 * @param object_count  The number of objects to hold
 * @param bucket_size   The size of a bucket
 *
 * @return The number of buckets required
 **/
static inline uint64_t compute_bucket_count(uint64_t object_count,
					    uint64_t bucket_size)
{
	uint64_t quotient = object_count / bucket_size;
	if ((object_count % bucket_size) > 0) {
		++quotient;
	}
	return quotient;
}

#endif // NUM_UTILS_H
