// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "hash-utils.h"

#include "permassert.h"
#include "uds.h"

/* Compute the number of bits required to store a given value. */
unsigned int compute_bits(unsigned int max_value)
{
	unsigned int bits = 0;

	while (max_value > 0) {
		max_value >>= 1;
		bits++;
	}

	return bits;
}

/* Special function wrapper required for compile-time assertions. */
void hash_utils_compile_time_assertions(void)
{
	STATIC_ASSERT(UDS_CHUNK_NAME_SIZE == 16);
}
