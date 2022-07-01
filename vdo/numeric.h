/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef NUMERIC_H
#define NUMERIC_H 1

#include "compiler.h"

#include <asm/unaligned.h>
#include <linux/kernel.h>


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
