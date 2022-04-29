// SPDX-License-Identifier: GPL-2.0-only
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
 */

#include "comparisons.h"

#include <asm/unaligned.h>

#include "permassert.h"

#include "constants.h"
#include "types.h"

/**
 * Compare blocks of memory for equality.
 *
 * This assumes the blocks are likely to be large; it's not well
 * optimized for comparing just a few bytes.  This is desirable
 * because the Linux kernel memcmp() routine on x86 is not well
 * optimized for large blocks, and the performance penalty turns out
 * to be significant if you're doing lots of 4KB comparisons.
 *
 * @param pointer_argument1  first data block
 * @param pointer_argument2  second data block
 * @param length             length of the data block
 *
 * @return true iff the two blocks are equal
 **/
bool
memory_equal(void *pointer_argument1, void *pointer_argument2, size_t length)
{
	byte *pointer1 = pointer_argument1;
	byte *pointer2 = pointer_argument2;

	while (length >= sizeof(uint64_t)) {
		/*
		 * get_unaligned is just for paranoia. (1) On x86_64 it is
		 * treated the same as an aligned access. (2) In this use case,
		 * one or both of the inputs will almost(?) always be aligned.
		 */
		if (get_unaligned((u64 *) pointer1) !=
		    get_unaligned((u64 *) pointer2)) {
			return false;
		}
		pointer1 += sizeof(uint64_t);
		pointer2 += sizeof(uint64_t);
		length -= sizeof(uint64_t);
	}
	while (length > 0) {
		if (*pointer1 != *pointer2) {
			return false;
		}
		pointer1++;
		pointer2++;
		length--;
	}
	return true;
}

/**
 * Check whether a data block is all zeros.
 *
 * @param block  The block to check
 *
 * @return true is all zeroes, false otherwise
 **/
bool is_zero_block(char *block)
{
	unsigned int word_count = VDO_BLOCK_SIZE / sizeof(uint64_t);
	unsigned int chunk_count = word_count / 8;

	/*
	 * Handle expected common case of even the first word being nonzero,
	 * without getting into the more expensive (for one iteration) loop
	 * below.
	 */
	if (get_unaligned((u64 *) block) != 0) {
		return false;
	}

	STATIC_ASSERT(VDO_BLOCK_SIZE % sizeof(uint64_t) == 0);

	/* Unroll to process 64 bytes at a time */
	while (chunk_count-- > 0) {
		uint64_t word0 = get_unaligned((u64 *) block);
		uint64_t word1 =
			get_unaligned((u64 *) (block + 1 * sizeof(uint64_t)));
		uint64_t word2 =
			get_unaligned((u64 *) (block + 2 * sizeof(uint64_t)));
		uint64_t word3 =
			get_unaligned((u64 *) (block + 3 * sizeof(uint64_t)));
		uint64_t word4 =
			get_unaligned((u64 *) (block + 4 * sizeof(uint64_t)));
		uint64_t word5 =
			get_unaligned((u64 *) (block + 5 * sizeof(uint64_t)));
		uint64_t word6 =
			get_unaligned((u64 *) (block + 6 * sizeof(uint64_t)));
		uint64_t word7 =
			get_unaligned((u64 *) (block + 7 * sizeof(uint64_t)));
		uint64_t or = (word0 | word1 | word2 | word3 | word4 | word5 |
			       word6 | word7);
		/* Prevent compiler from using 8*(cmp;jne). */
		__asm__ __volatile__("" : : "g"(or));
		if (or != 0) {
			return false;
		}
		block += 8 * sizeof(uint64_t);
	}
	word_count %= 8;

	/*
	 * Unroll to process 8 bytes at a time. 
	 * (Is this still worthwhile?) 
	 */
	while (word_count-- > 0) {
		if (get_unaligned((u64 *) block) != 0) {
			return false;
		}
		block += sizeof(uint64_t);
	}
	return true;
}

