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
 * $Id: //eng/uds-releases/krusty/src/uds/numeric.h#5 $
 */

#ifndef NUMERIC_H
#define NUMERIC_H 1

#include "compiler.h"

#include <asm/unaligned.h>

/**
 * Find the minimum of two ints.
 *
 * @param a The first int
 * @param b The second int
 *
 * @return The lesser of a and b
 **/
static INLINE int __must_check min_int(int a, int b)
{
	return ((a < b) ? a : b);
}

/**
 * Find the maximum of two ints.
 *
 * @param a The first int
 * @param b The second int
 *
 * @return The greater of a and b
 **/
static INLINE int __must_check max_int(int a, int b)
{
	return ((a > b) ? a : b);
}

/**
 * Find the maximum of two unsigned ints.
 *
 * @param a The first value
 * @param b The second value
 *
 * @return The greater of a and b
 **/
static INLINE unsigned int __must_check max_uint(unsigned int a,
						 unsigned int b)
{
	return ((a > b) ? a : b);
}

/**
 * Find the maximum of two signed longs.
 *
 * @param a The first int
 * @param b The second int
 *
 * @return The greater of a and b
 **/
static INLINE long __must_check max_long(long a, long b)
{
	return ((a > b) ? a : b);
}

/**
 * Find the maximum of two unsigned longs.
 *
 * @param a The first int
 * @param b The second int
 *
 * @return The greater of a and b
 **/
static INLINE unsigned long __must_check max_ulong(unsigned long a,
						   unsigned long b)
{
	return ((a > b) ? a : b);
}

/**
 * Find the minimum of two size_ts.
 *
 * @param a The first size_t
 * @param b The second size_t
 *
 * @return The lesser of a and b
 **/
static INLINE size_t __must_check min_size_t(size_t a, size_t b)
{
	return ((a < b) ? a : b);
}

/**
 * Find the maximum of two size_ts.
 *
 * @param a The first size_t
 * @param b The second size_t
 *
 * @return The greater of a and b
 **/
static INLINE size_t __must_check max_size_t(size_t a, size_t b)
{
	return ((a > b) ? a : b);
}

/**
 * Find the minimum of two uint64_ts.
 *
 * @param a The first uint64_t
 * @param b The second uint64_t
 *
 * @return The lesser of a and b
 **/
static INLINE uint64_t __must_check max_uint64(uint64_t a, uint64_t b)
{
	return ((a < b) ? a : b);
}

/**
 * Extract a 64 bit unsigned big-endian number from a buffer at a
 * specified offset.  The offset will be advanced to the first byte
 * after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to extract
 * @param decoded A pointer to hold the extracted number
 **/
static INLINE void decode_uint64_be(const uint8_t *buffer,
				    size_t *offset,
				    uint64_t *decoded)
{
	*decoded = get_unaligned_be64(buffer + *offset);
	*offset += sizeof(uint64_t);
}

/**
 * Encode a 64 bit unsigned number into a buffer at a given offset
 * using a big-endian representation. The offset will be advanced to
 * first byte after the encoded number.
 *
 * @param data      The buffer to encode into
 * @param offset    A pointer to the offset at which to start encoding
 * @param to_encode The number to encode
 **/
static INLINE void encode_uint64_be(uint8_t *data,
				    size_t *offset,
				    uint64_t to_encode)
{
	put_unaligned_be64(to_encode, data + *offset);
	*offset += sizeof(uint64_t);
}

/**
 * Extract a 32 bit unsigned big-endian number from a buffer at a
 * specified offset.  The offset will be advanced to the first byte
 * after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to extract
 * @param decoded A pointer to hold the extracted number
 **/
static INLINE void decode_uint32_be(const uint8_t *buffer,
				    size_t *offset,
				    uint32_t *decoded)
{
	*decoded = get_unaligned_be32(buffer + *offset);
	*offset += sizeof(uint32_t);
}

/**
 * Encode a 32 bit number into a buffer at a given offset using a
 * big-endian representation. The offset will be advanced to first byte
 * after the encoded number.
 *
 * @param data      The buffer to encode into
 * @param offset    A pointer to the offset at which to start encoding
 * @param to_encode The number to encode
 **/
static INLINE void encode_uint32_be(uint8_t *data,
				    size_t *offset,
				    uint32_t to_encode)
{
	put_unaligned_be32(to_encode, data + *offset);
	*offset += sizeof(uint32_t);
}

/**
 * Extract a 16 bit, big-endian number from a buffer at a specified offset.
 * The offset will be advanced to the first byte after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to
 *                extract
 * @param decoded A pointer to hold the extracted number
 **/
static INLINE void decode_uint16_be(const uint8_t *buffer,
				    size_t *offset,
				    uint16_t *decoded)
{
	*decoded = get_unaligned_be16(buffer + *offset);
	*offset += sizeof(uint16_t);
}

/**
 * Store a 16 bit number in a buffer in
 * big-endian representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static INLINE void store_uint16_be(uint8_t *data, uint16_t num)
{
	put_unaligned_be16(num, data);
}

/**
 * Encode a 16 bit number into a buffer at a given offset using a
 * big-endian representation. The offset will be advanced to first byte
 * after the encoded number.
 *
 * @param data      The buffer to encode into
 * @param offset    A pointer to the offset at which to start encoding
 * @param to_encode The number to encode
 **/
static INLINE void encode_uint16_be(uint8_t *data,
				    size_t *offset,
				    uint16_t to_encode)
{
	put_unaligned_be16(to_encode, data + *offset);
	*offset += sizeof(uint16_t);
}

/**
 * Extract a 64 bit signed little-endian number from a buffer at a
 * specified offset.  The offset will be advanced to the first byte
 * after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to extract
 * @param decoded A pointer to hold the extracted number
 **/
static INLINE void decode_int64_le(const uint8_t *buffer,
				   size_t *offset,
				   int64_t *decoded)
{
	*decoded = get_unaligned_le64(buffer + *offset);
	*offset += sizeof(int64_t);
}

/**
 * Encode a 64 bit signed number into a buffer at a given offset using
 * a little-endian representation. The offset will be advanced to
 * first byte after the encoded number.
 *
 * @param data      The buffer to encode into
 * @param offset    A pointer to the offset at which to start encoding
 * @param to_encode The number to encode
 **/
static INLINE void encode_int64_le(uint8_t *data,
				   size_t *offset,
				   int64_t to_encode)
{
	put_unaligned_le64(to_encode, data + *offset);
	*offset += sizeof(int64_t);
}

/**
 * Extract a 64 bit unsigned little-endian number from a buffer at a
 * specified offset.  The offset will be advanced to the first byte
 * after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to extract
 * @param decoded A pointer to hold the extracted number
 **/
static INLINE void decode_uint64_le(const uint8_t *buffer,
				    size_t *offset,
				    uint64_t *decoded)
{
	*decoded = get_unaligned_le64(buffer + *offset);
	*offset += sizeof(uint64_t);
}

/**
 * Encode a 64 bit unsigned number into a buffer at a given offset
 * using a little-endian representation. The offset will be advanced
 * to first byte after the encoded number.
 *
 * @param data      The buffer to encode into
 * @param offset    A pointer to the offset at which to start encoding
 * @param to_encode The number to encode
 **/
static INLINE void encode_uint64_le(uint8_t *data,
				    size_t *offset,
				    uint64_t to_encode)
{
	put_unaligned_le64(to_encode, data + *offset);
	*offset += sizeof(uint64_t);
}

/**
 * Extract a 32 bit signed little-endian number from a buffer at a
 * specified offset.  The offset will be advanced to the first byte
 * after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to extract
 * @param decoded A pointer to hold the extracted number
 **/
static INLINE void decode_int32_le(const uint8_t *buffer,
				   size_t *offset,
				   int32_t *decoded)
{
	*decoded = get_unaligned_le32(buffer + *offset);
	*offset += sizeof(int32_t);
}

/**
 * Encode a 32 bit signed number into a buffer at a given offset using
 * a little-endian representation. The offset will be advanced to
 * first byte after the encoded number.
 *
 * @param data      The buffer to encode into
 * @param offset    A pointer to the offset at which to start encoding
 * @param to_encode The number to encode
 **/
static INLINE void encode_int32_le(uint8_t *data,
				   size_t *offset,
				   int32_t to_encode)
{
	put_unaligned_le32(to_encode, data + *offset);
	*offset += sizeof(int32_t);
}

/**
 * Extract a 32 bit unsigned little-endian number from a buffer at a
 * specified offset.  The offset will be advanced to the first byte
 * after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to extract
 * @param decoded A pointer to hold the extracted number
 **/
static INLINE void decode_uint32_le(const uint8_t *buffer,
				    size_t *offset,
				    uint32_t *decoded)
{
	*decoded = get_unaligned_le32(buffer + *offset);
	*offset += sizeof(uint32_t);
}

/**
 * Encode a 32 bit unsigned number into a buffer at a given offset
 * using a little-endian representation. The offset will be advanced
 * to first byte after the encoded number.
 *
 * @param data      The buffer to encode into
 * @param offset    A pointer to the offset at which to start encoding
 * @param to_encode The number to encode
 **/
static INLINE void encode_uint32_le(uint8_t *data,
				    size_t *offset,
				    uint32_t to_encode)
{
	put_unaligned_le32(to_encode, data + *offset);
	*offset += sizeof(uint32_t);
}

/**
 * Extract a 16 bit unsigned little-endian number from a buffer at a
 * specified offset.  The offset will be advanced to the first byte
 * after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to
 *                extract
 * @param decoded A pointer to hold the extracted number
 **/
static INLINE void decode_uint16_le(const uint8_t *buffer,
				    size_t *offset,
				    uint16_t *decoded)
{
	*decoded = get_unaligned_le16(buffer + *offset);
	*offset += sizeof(uint16_t);
}

/**
 * Encode a 16 bit unsigned number into a buffer at a given offset
 * using a little-endian representation. The offset will be advanced
 * to first byte after the encoded number.
 *
 * @param data      The buffer to encode into
 * @param offset    A pointer to the offset at which to start encoding
 * @param to_encode The number to encode
 **/
static INLINE void encode_uint16_le(uint8_t *data,
				    size_t *offset,
				    uint16_t to_encode)
{
	put_unaligned_le16(to_encode, data + *offset);
	*offset += sizeof(uint16_t);
}

/**
 * Special function wrapper required for compile-time assertions. This
 * function will fail to compile if any of the uint*_t types are not of the
 * size we expect. This function should never be called.
 **/
void numeric_compile_time_assertions(void);

#endif /* NUMERIC_H */
