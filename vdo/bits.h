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
 */

#ifndef BITS_H
#define BITS_H 1

#include "compiler.h"
#include "numeric.h"
#include "type-defs.h"

/*
 * These bit stream and bit field utility routines are used for the delta
 * indexes, which are not byte-aligned.
 *
 * Bits and bytes are numbered in little endian order. Within a byte, bit 0
 * is the least significant bit (0x1), and bit 7 is the most significant bit
 * (0x80). Within a bit stream, bit 7 is the most signficant bit of byte 0,
 * and bit 8 is the least significant bit of byte 1. Within a byte array, a
 * byte's number corresponds to its index in the array.
 *
 * This implementation assumes that the native machine is little endian, and
 * that performance is very important.
 */

/*
 * This is the largest field size supported by get_field() and set_field().
 * Any field that is larger is not guaranteed to fit in a single byte-aligned
 * uint32_t.
 */
enum {
	MAX_FIELD_BITS = (sizeof(uint32_t) - 1) * CHAR_BIT + 1,
};

/*
 * This is the number of guard bytes needed at the end of the memory byte
 * array when using the bit utilities. 3 bytes are needed when get_field() and
 * set_field() access a field, because they will access some extra bytes past
 * the end of the field. 7 bytes are needed when get_big_field() and
 * set_big_field() access a big field, for the same reason. Note that
 * move_bits() calls get_big_field() and set_big_field(). The definition is
 * written to make it clear how it is derived.
 */
enum {
	POST_FIELD_GUARD_BYTES = sizeof(uint64_t) - 1,
};

/*
 * Get a bit field from a bit stream.
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 *
 * @return the bit field
 */
static INLINE unsigned int
get_field(const byte *memory, uint64_t offset, int size)
{
	const void *addr = memory + offset / CHAR_BIT;

	return ((get_unaligned_le32(addr) >> (offset % CHAR_BIT)) &
		((1 << size) - 1));
}

/*
 * Set a bit field in a bit stream.
 *
 * @param value   The value to put into the field
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 */
static INLINE void
set_field(unsigned int value, byte *memory, uint64_t offset, int size)
{
	void *addr = memory + offset / CHAR_BIT;
	int shift = offset % CHAR_BIT;
	uint32_t data = get_unaligned_le32(addr);

	data &= ~(((1 << size) - 1) << shift);
	data |= value << shift;
	put_unaligned_le32(data, addr);
}

/*
 * Set a bit field in a bit stream to all ones.
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 */
static INLINE void set_one(byte *memory, uint64_t offset, int size)
{
	if (size > 0) {
		byte *addr = memory + offset / CHAR_BIT;
		int shift = offset % CHAR_BIT;
		int count = size + shift > CHAR_BIT ? CHAR_BIT - shift : size;

		*addr++ |= ((1 << count) - 1) << shift;
		for (size -= count; size > CHAR_BIT; size -= CHAR_BIT) {
			*addr++ = 0xFF;
		}

		if (size > 0) {
			*addr |= ~(0xFF << size);
		}
	}
}

/*
 * Set a bit field in a bit stream to all zeros.
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 */
static INLINE void set_zero(byte *memory, uint64_t offset, int size)
{
	if (size > 0) {
		byte *addr = memory + offset / CHAR_BIT;
		int shift = offset % CHAR_BIT;
		int count = size + shift > CHAR_BIT ? CHAR_BIT - shift : size;

		*addr++ &= ~(((1 << count) - 1) << shift);
		for (size -= count; size > CHAR_BIT; size -= CHAR_BIT) {
			*addr++ = 0;
		}

		if (size > 0) {
			*addr &= 0xFF << size;
		}
	}
}

void get_bytes(const byte *memory,
	       uint64_t offset,
	       byte *destination,
	       int size);

void set_bytes(byte *memory, uint64_t offset, const byte *source, int size);

void move_bits(const byte *s_memory,
	       uint64_t source,
	       byte *d_memory,
	       uint64_t destination,
	       int size);

bool __must_check same_bits(const byte *mem1,
			    uint64_t offset1,
			    const byte *mem2,
			    uint64_t offset2,
			    int size);

#endif /* BITS_H */
