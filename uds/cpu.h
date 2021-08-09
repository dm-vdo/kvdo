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
 * $Id: //eng/uds-releases/lisa/src/uds/cpu.h#1 $
 */

#ifndef CPU_H
#define CPU_H

#include "compiler.h"
#include "typeDefs.h"

/**
 * The number of bytes in a CPU cache line. In the future, we'll probably need
 * to move this to a processor-specific file or discover it at compilation
 * time (or runtime, if sufficiently heterogeneous), but this will do for now.
 * (Must be a \#define since enums are not proper compile-time constants.)
 **/
#ifdef __PPC__
// N.B.: Some PPC processors have smaller cache lines.
#define CACHE_LINE_BYTES 128
#elif defined(__s390x__)
#define CACHE_LINE_BYTES 256
#elif defined(__x86_64__) || defined(__aarch64__)
#define CACHE_LINE_BYTES 64
#else
#error "unknown cache line size"
#endif

/**
 * Minimize cache-miss latency by moving data into a CPU cache before it is
 * accessed.
 *
 * @param address    the address to fetch (may be invalid)
 * @param for_write  must be constant at compile time--false if
 *                   for reading, true if for writing
 **/
static INLINE void prefetch_address(const void *address, bool for_write)
{
	// for_write won't won't be a constant if we are compiled with
	// optimization turned off, in which case prefetching really doesn't
	// matter.
	if (__builtin_constant_p(for_write)) {
		__builtin_prefetch(address, for_write);
	}
}

/**
 * Minimize cache-miss latency by moving a range of addresses into a
 * CPU cache before they are accessed.
 *
 * @param start      the starting address to fetch (may be invalid)
 * @param size       the number of bytes in the address range
 * @param for_write  must be constant at compile time--false if
 *                   for reading, true if for writing
 **/
static INLINE void
prefetch_range(const void *start, unsigned int size, bool for_write)
{
	// Count the number of cache lines to fetch, allowing for the address
	// range to span an extra cache line boundary due to address alignment.
	const char *address = (const char *) start;
	unsigned int offset = ((uintptr_t) address % CACHE_LINE_BYTES);
	unsigned int cache_lines = (1 + ((size + offset) / CACHE_LINE_BYTES));
	while (cache_lines-- > 0) {
		prefetch_address(address, for_write);
		address += CACHE_LINE_BYTES;
	}
}

#endif /* CPU_H */
