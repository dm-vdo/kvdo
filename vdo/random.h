/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RANDOM_H
#define RANDOM_H

#include <linux/random.h>

#include "compiler.h"
#include "type-defs.h"

/**
 * Get random unsigned integer in a given range
 *
 * @param lo  Minimum unsigned integer value
 * @param hi  Maximum unsigned integer value
 *
 * @return unsigned integer in the interval [lo,hi]
 **/
unsigned int random_in_range(unsigned int lo, unsigned int hi);

/**
 * Special function wrapper required for compile-time assertions. This
 * function will fail to compile if RAND_MAX is not of the form 2^n - 1.
 **/
void random_compile_time_assertions(void);

/**
 * Fill bytes with random data.
 *
 * @param ptr   where to store bytes
 * @param len   number of bytes to write
 **/
static INLINE void fill_randomly(void *ptr, size_t len)
{
	get_random_bytes(ptr, len);
}

#define RAND_MAX 2147483647

/**
 * Random number generator
 *
 * @return a random number in the rand 0 to RAND_MAX
 **/
static INLINE long random(void)
{
	long value;
	fill_randomly(&value, sizeof(value));
	return value & RAND_MAX;
}

#endif /* RANDOM_H */
