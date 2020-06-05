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
 * $Id: //eng/uds-releases/krusty/src/uds/bits.c#4 $
 */

#include "bits.h"

#include "compiler.h"

/**
 * This is the largest field size supported by get_big_field & set_big_field.
 * Any field that is larger is not guaranteed to fit in a single, byte
 * aligned uint64_t.
 **/
enum { MAX_BIG_FIELD_BITS = (sizeof(uint64_t) - 1) * CHAR_BIT + 1 };

/**
 * Get a big bit field from a bit stream
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 *
 * @return the bit field
 **/
static INLINE uint64_t get_big_field(const byte *memory,
				     uint64_t offset,
				     int size)
{
	const void *addr = memory + offset / CHAR_BIT;
	return (get_unaligned_le64(addr) >> (offset % CHAR_BIT)) &
	       ((1UL << size) - 1);
}

/**
 * Set a big bit field in a bit stream
 *
 * @param value   The value to put into the field
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 *
 * @return the bit field
 **/
static INLINE void
set_big_field(uint64_t value, byte *memory, uint64_t offset, int size)
{
	void *addr = memory + offset / CHAR_BIT;
	int shift = offset % CHAR_BIT;
	uint64_t data = get_unaligned_le64(addr);
	data &= ~(((1UL << size) - 1) << shift);
	data |= value << shift;
	put_unaligned_le64(data, addr);
}

/***********************************************************************/
void get_bytes(const byte *memory, uint64_t offset, byte *destination, int size)
{
	const byte *addr = memory + offset / CHAR_BIT;
	int shift = offset % CHAR_BIT;
	while (--size >= 0) {
		*destination++ = get_unaligned_le16(addr++) >> shift;
	}
}

/***********************************************************************/
void set_bytes(byte *memory, uint64_t offset, const byte *source, int size)
{
	byte *addr = memory + offset / CHAR_BIT;
	int shift = offset % CHAR_BIT;
	uint16_t mask = ~((uint16_t) 0xFF << shift);
	while (--size >= 0) {
		uint16_t data = (get_unaligned_le16(addr) & mask) |
				(*source++ << shift);
		put_unaligned_le16(data, addr++);
	}
}

/***********************************************************************/
void move_bits(const byte *s_memory,
	       uint64_t source,
	       byte *d_memory,
	       uint64_t destination,
	       int size)
{
	enum { UINT32_BIT = sizeof(uint32_t) * CHAR_BIT };
	if (size > MAX_BIG_FIELD_BITS) {
		if (source > destination) {
			// This is a large move from a higher to a lower
			// address.  We move the lower addressed bits first.
			// Start by moving one field that ends on a destination
			// int boundary
			int count =
				MAX_BIG_FIELD_BITS -
				(destination + MAX_BIG_FIELD_BITS) % UINT32_BIT;
			uint64_t field =
				get_big_field(s_memory, source, count);
			set_big_field(field, d_memory, destination, count);
			source += count;
			destination += count;
			size -= count;
			// Now do the main loop to copy 32 bit chunks that are
			// int-aligned at the destination.
			int offset = source % UINT32_BIT;
			const byte *src =
				s_memory + (source - offset) / CHAR_BIT;
			byte *dest = d_memory + destination / CHAR_BIT;
			while (size > MAX_BIG_FIELD_BITS) {
				put_unaligned_le32(get_unaligned_le64(src) >>
							   offset,
						   dest);
				src += sizeof(uint32_t);
				dest += sizeof(uint32_t);
				source += UINT32_BIT;
				destination += UINT32_BIT;
				size -= UINT32_BIT;
			}
		} else {
			// This is a large move from a lower to a higher
			// address.  We move the higher addressed bits first.
			// Start by moving one field that begins on a
			// destination int boundary
			int count = (destination + size) % UINT32_BIT;
			if (count > 0) {
				size -= count;
				uint64_t field = get_big_field(s_memory,
							       source + size,
							       count);
				set_big_field(field,
					      d_memory,
					      destination + size,
					      count);
			}
			// Now do the main loop to copy 32 bit chunks that are
			// int-aligned at the destination.
			int offset = (source + size) % UINT32_BIT;
			const byte *src =
				s_memory + (source + size - offset) / CHAR_BIT;
			byte *dest =
				d_memory + (destination + size) / CHAR_BIT;
			while (size > MAX_BIG_FIELD_BITS) {
				src -= sizeof(uint32_t);
				dest -= sizeof(uint32_t);
				size -= UINT32_BIT;
				put_unaligned_le32(get_unaligned_le64(src) >>
							   offset,
						   dest);
			}
		}
	}
	// Finish up by doing the last chunk, which can have any arbitrary
	// alignment
	if (size > 0) {
		uint64_t field = get_big_field(s_memory, source, size);
		set_big_field(field, d_memory, destination, size);
	}
}

/***********************************************************************/
bool same_bits(const byte *mem1,
	       uint64_t offset1,
	       const byte *mem2,
	       uint64_t offset2,
	       int size)
{
	while (size >= MAX_FIELD_BITS) {
		unsigned int field1 = get_field(mem1, offset1, MAX_FIELD_BITS);
		unsigned int field2 = get_field(mem2, offset2, MAX_FIELD_BITS);
		if (field1 != field2)
			return false;
		offset1 += MAX_FIELD_BITS;
		offset2 += MAX_FIELD_BITS;
		size -= MAX_FIELD_BITS;
	}
	if (size > 0) {
		unsigned int field1 = get_field(mem1, offset1, size);
		unsigned int field2 = get_field(mem2, offset2, size);
		if (field1 != field2)
			return false;
	}
	return true;
}
