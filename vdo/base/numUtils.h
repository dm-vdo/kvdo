/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/numUtils.h#7 $
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
static inline bool is_power_of_two(uint64_t n)
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
 * Find the minimum of two physical block numbers.
 **/
static inline physical_block_number_t __must_check
min_block(physical_block_number_t a, physical_block_number_t b)
{
	return (a < b) ? a : b;
}

/**
 * Find the maximum of two physical block numbers.
 **/
static inline physical_block_number_t __must_check
max_block(physical_block_number_t a, physical_block_number_t b)
{
	return (a > b) ? a : b;
}

/**
 * Find the minimum of two block counts.
 **/
static inline block_count_t __must_check
min_block_count(block_count_t a, block_count_t b)
{
	return (a < b) ? a : b;
}

/**
 * Find the maximum of two block counts.
 **/
static inline block_count_t __must_check
max_block_count(block_count_t a, block_count_t b)
{
	return (a > b) ? a : b;
}

/**
 * Find the minimum of two sequence numbers.
 **/
static inline sequence_number_t __must_check
min_sequence_number(sequence_number_t a, sequence_number_t b)
{
	return (a < b) ? a : b;
}

/**
 * Return the minimum of two page counts.
 **/
static inline page_count_t __must_check
min_page_count(page_count_t a, page_count_t b)
{
	return (a < b) ? a : b;
}

/**
 * Return the maximum of two page counts.
 **/
static inline page_count_t __must_check
max_page_count(page_count_t a, page_count_t b)
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
static inline size_t __must_check
round_up_to_multiple_size_t(size_t number, size_t quantum)
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
static inline uint64_t __must_check
round_up_to_multiple_u_int64_t(uint64_t number, uint64_t quantum)
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
