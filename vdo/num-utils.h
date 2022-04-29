/* SPDX-License-Identifier: GPL-2.0-only */
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
 * THIS FILE IS A CANDIDATE FOR THE EVENTUAL UTILITY LIBRARY.
 */

#ifndef NUM_UTILS_H
#define NUM_UTILS_H

#include "numeric.h"

#include "types.h"

#include <linux/log2.h>
#include <linux/math.h>


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

#endif /* NUM_UTILS_H */
