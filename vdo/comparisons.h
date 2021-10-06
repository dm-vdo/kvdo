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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/comparisons.h#1 $
 */

#ifndef COMPARISONS_H
#define MEMORY_EQUQL_H

#include <asm/unaligned.h>

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
bool __must_check
memory_equal(void *pointer_argument1, void *pointer_argument2, size_t length);

/**
 * Check whether a data block is all zeros.
 *
 * @param block  The block to check
 *
 * @return true is all zeroes, false otherwise
 **/
bool __must_check is_zero_block(char *block);

#endif // COMPARISONS_H
