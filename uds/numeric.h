/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/gloria/src/uds/numeric.h#5 $
 */

#ifndef NUMERIC_H
#define NUMERIC_H 1

#include "compiler.h"
#include "numericDefs.h"
#include "typeDefs.h"

#if !defined(__ORDER_LITTLE_ENDIAN__) || !defined(__ORDER_BIG_ENDIAN__) \
  || !defined(__BYTE_ORDER__)
#error "GCC byte order macros not defined?"
#endif

/*
 * Define a type describing an integer value that is only byte-aligned
 * and may explicitly alias other types.  GCC keeps getting better
 * about type-based alias analysis (both for optimization and for
 * warnings), so simply casting a pointer to pointer-to-uintXX_t isn't
 * good enough.
 *
 * C is okay with defining the structures directly in a cast, but
 * C++ is not, and we use this header in some C++ code internally.
 */
#define UNALIGNED_WRAPPER(TYPE)                 \
  unaligned_wrap_##TYPE
#define UNALIGNED_WRAPPER_DEF(TYPE)                                 \
  typedef struct __attribute__((packed, may_alias)) { TYPE value; } \
  UNALIGNED_WRAPPER(TYPE)
UNALIGNED_WRAPPER_DEF(int64_t);
UNALIGNED_WRAPPER_DEF(uint64_t);
UNALIGNED_WRAPPER_DEF(int32_t);
UNALIGNED_WRAPPER_DEF(uint32_t);
UNALIGNED_WRAPPER_DEF(uint16_t);

#define GET_UNALIGNED(TYPE,ADDR)                        \
  (((const UNALIGNED_WRAPPER(TYPE) *)(ADDR))->value)
#define PUT_UNALIGNED(TYPE,ADDR,VALUE)                  \
  (((UNALIGNED_WRAPPER(TYPE) *)(ADDR))->value = (VALUE))

/**
 * Find the minimum of two ints.
 *
 * @param a The first int
 * @param b The second int
 *
 * @return The lesser of a and b
 **/
__attribute__((warn_unused_result))
static INLINE int minInt(int a, int b)
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
__attribute__((warn_unused_result))
static INLINE int maxInt(int a, int b)
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
__attribute__((warn_unused_result))
static INLINE unsigned int maxUInt(unsigned int a, unsigned int b)
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
__attribute__((warn_unused_result))
static INLINE long maxLong(long a, long b)
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
__attribute__((warn_unused_result))
static INLINE unsigned long maxULong(unsigned long a, unsigned long b)
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
__attribute__((warn_unused_result))
static INLINE size_t minSizeT(size_t a, size_t b)
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
__attribute__((warn_unused_result))
static INLINE size_t maxSizeT(size_t a, size_t b)
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
__attribute__((warn_unused_result))
static INLINE uint64_t minUInt64(uint64_t a, uint64_t b)
{
  return ((a < b) ? a : b);
}

/**
 * Determine the greatest common divisor of two numbers.
 *
 * @param a             a number
 * @param b             another number
 *
 * @return              the gcd of a and b
 **/
uint64_t greatestCommonDivisor(uint64_t a, uint64_t b);

/**
 * Determine the least common multiple of two numbers.
 *
 * @param a             a number
 * @param b             another number
 *
 * @return              the lcm of a and b
 **/
uint64_t leastCommonMultiple(uint64_t a, uint64_t b);

/**
 * Multiply two uint64_t and check for overflow. Does division.
 **/
bool multiplyWouldOverflow(uint64_t a, uint64_t b);

/**
 * Extract a 64 bit unsigned number from a buffer stored in
 * big-endian representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static INLINE uint64_t getUInt64BE(const byte* data)
{
  uint64_t num = GET_UNALIGNED(uint64_t, data);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  return num;
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
static INLINE void decodeUInt64BE(const byte *buffer,
                                  size_t     *offset,
                                  uint64_t   *decoded)
{
  *decoded = getUInt64BE(buffer + *offset);
  *offset += sizeof(uint64_t);
}

/**
 * Store a 64 bit unsigned number in a buffer in
 * big-endian representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static INLINE void storeUInt64BE(byte* data, uint64_t num)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  PUT_UNALIGNED(uint64_t, data, num);
}

/**
 * Encode a 64 bit unsigned number into a buffer at a given offset
 * using a big-endian representation. The offset will be advanced to
 * first byte after the encoded number.
 *
 * @param data     The buffer to encode into
 * @param offset   A pointer to the offset at which to start encoding
 * @param toEncode The number to encode
 **/
static INLINE void encodeUInt64BE(byte     *data,
                                  size_t   *offset,
                                  uint64_t  toEncode)
{
  storeUInt64BE(data + *offset, toEncode);
  *offset += sizeof(uint64_t);
}

/**
 * Extract a 32 bit unsigned number from a buffer stored in big-endian
 * representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static INLINE uint32_t getUInt32BE(const byte* data)
{
  uint32_t num = GET_UNALIGNED(uint32_t, data);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap32(num);
#endif
  return num;
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
static INLINE void decodeUInt32BE(const byte *buffer,
                                  size_t     *offset,
                                  uint32_t   *decoded)
{
  *decoded = getUInt32BE(buffer + *offset);
  *offset += sizeof(uint32_t);
}

/**
 * Store a 32 bit number in a buffer in
 * big-endian representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static INLINE void storeUInt32BE(byte* data, uint32_t num)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap32(num);
#endif
  PUT_UNALIGNED(uint32_t, data, num);
}

/**
 * Encode a 32 bit number into a buffer at a given offset using a
 * big-endian representation. The offset will be advanced to first byte
 * after the encoded number.
 *
 * @param data     The buffer to encode into
 * @param offset   A pointer to the offset at which to start encoding
 * @param toEncode The number to encode
 **/
static INLINE void encodeUInt32BE(byte     *data,
                                  size_t   *offset,
                                  uint32_t  toEncode)
{
  storeUInt32BE(data + *offset, toEncode);
  *offset += sizeof(uint32_t);
}

/**
 * Extract a 16 bit number from a buffer stored in
 * big-endian representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static INLINE uint16_t getUInt16BE(const byte* data)
{
  uint16_t num = GET_UNALIGNED(uint16_t, data);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  num = bswap_16(num);
#endif
  return num;
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
static INLINE void decodeUInt16BE(const byte *buffer,
                                  size_t     *offset,
                                  uint16_t   *decoded)
{
  *decoded = getUInt16BE(buffer + *offset);
  *offset += sizeof(uint16_t);
}

/**
 * Store a 16 bit number in a buffer in
 * big-endian representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static INLINE void storeUInt16BE(byte* data, uint16_t num)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  num = bswap_16(num);
#endif
  PUT_UNALIGNED(uint16_t, data, num);
}

/**
 * Encode a 16 bit number into a buffer at a given offset using a
 * big-endian representation. The offset will be advanced to first byte
 * after the encoded number.
 *
 * @param data     The buffer to encode into
 * @param offset   A pointer to the offset at which to start encoding
 * @param toEncode The number to encode
 **/
static INLINE void encodeUInt16BE(byte     *data,
                                  size_t   *offset,
                                  uint16_t  toEncode)
{
  storeUInt16BE(data + *offset, toEncode);
  *offset += sizeof(uint16_t);
}

/**
 * Extract a 64 bit signed number from a buffer stored in
 * little-endian representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static INLINE int64_t getInt64LE(const byte* data)
{
  int64_t num = GET_UNALIGNED(int64_t, data);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  return num;
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
static INLINE void decodeInt64LE(const byte *buffer,
                                 size_t     *offset,
                                 int64_t   *decoded)
{
  *decoded = getInt64LE(buffer + *offset);
  *offset += sizeof(int64_t);
}

/**
 * Store a signed 64 bit number in a buffer in little-endian
 * representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static INLINE void storeInt64LE(byte* data, int64_t num)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  PUT_UNALIGNED(int64_t, data, num);
}

/**
 * Encode a 64 bit signed number into a buffer at a given offset using
 * a little-endian representation. The offset will be advanced to
 * first byte after the encoded number.
 *
 * @param data     The buffer to encode into
 * @param offset   A pointer to the offset at which to start encoding
 * @param toEncode The number to encode
 **/
static INLINE void encodeInt64LE(byte    *data,
                                 size_t  *offset,
                                 int64_t  toEncode)
{
  storeInt64LE(data + *offset, toEncode);
  *offset += sizeof(int64_t);
}

/**
 * Extract a 64 bit number from a buffer stored in
 * little-endian representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static INLINE uint64_t getUInt64LE(const byte* data)
{
  uint64_t num = GET_UNALIGNED(uint64_t, data);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  return num;
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
static INLINE void decodeUInt64LE(const byte *buffer,
                                  size_t     *offset,
                                  uint64_t   *decoded)
{
  *decoded = getUInt64LE(buffer + *offset);
  *offset += sizeof(uint64_t);
}

/**
 * Store a 64 bit unsigned number in a buffer in little-endian
 * representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static INLINE void storeUInt64LE(byte* data, uint64_t num)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  PUT_UNALIGNED(uint64_t, data, num);
}

/**
 * Encode a 64 bit unsigned number into a buffer at a given offset
 * using a little-endian representation. The offset will be advanced
 * to first byte after the encoded number.
 *
 * @param data     The buffer to encode into
 * @param offset   A pointer to the offset at which to start encoding
 * @param toEncode The number to encode
 **/
static INLINE void encodeUInt64LE(byte     *data,
                                  size_t   *offset,
                                  uint64_t  toEncode)
{
  storeUInt64LE(data + *offset, toEncode);
  *offset += sizeof(uint64_t);
}

/**
 * Extract a 32 bit signed number from a buffer stored in
 * little-endian representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static INLINE int32_t getInt32LE(const byte* data)
{
  int32_t num = GET_UNALIGNED(int32_t, data);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap32(num);
#endif
  return num;
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
static INLINE void decodeInt32LE(const byte *buffer,
                                 size_t     *offset,
                                 int32_t   *decoded)
{
  *decoded = getInt32LE(buffer + *offset);
  *offset += sizeof(int32_t);
}

/**
 * Store a signed 32 bit number in a buffer in little-endian
 * representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static INLINE void storeInt32LE(byte* data, int32_t num)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap32(num);
#endif
  PUT_UNALIGNED(int32_t, data, num);
}

/**
 * Encode a 32 bit signed number into a buffer at a given offset using
 * a little-endian representation. The offset will be advanced to
 * first byte after the encoded number.
 *
 * @param data     The buffer to encode into
 * @param offset   A pointer to the offset at which to start encoding
 * @param toEncode The number to encode
 **/
static INLINE void encodeInt32LE(byte    *data,
                                 size_t  *offset,
                                 int32_t  toEncode)
{
  storeInt32LE(data + *offset, toEncode);
  *offset += sizeof(int32_t);
}

/**
 * Extract a 32 bit unsigned number from a buffer stored in
 * little-endian representation.

 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static INLINE uint32_t getUInt32LE(const byte* data)
{
  uint32_t num = GET_UNALIGNED(uint32_t, data);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap32(num);
#endif
  return num;
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
static INLINE void decodeUInt32LE(const byte *buffer,
                                  size_t     *offset,
                                  uint32_t   *decoded)
{
  *decoded = getUInt32LE(buffer + *offset);
  *offset += sizeof(uint32_t);
}

/**
 * Store a 32 bit unsigned number in a buffer in little-endian
 * representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static INLINE void storeUInt32LE(byte* data, uint32_t num)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap32(num);
#endif
  PUT_UNALIGNED(uint32_t, data, num);
}

/**
 * Encode a 32 bit unsigned number into a buffer at a given offset
 * using a little-endian representation. The offset will be advanced
 * to first byte after the encoded number.
 *
 * @param data     The buffer to encode into
 * @param offset   A pointer to the offset at which to start encoding
 * @param toEncode The number to encode
 **/
static INLINE void encodeUInt32LE(byte     *data,
                                  size_t   *offset,
                                  uint32_t  toEncode)
{
  storeUInt32LE(data + *offset, toEncode);
  *offset += sizeof(uint32_t);
}

/**
 * Extract a 16 bit number from a buffer stored in
 * little-endian representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static INLINE uint16_t getUInt16LE(const byte* data)
{
  uint16_t num = GET_UNALIGNED(uint16_t, data);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = bswap_16(num);
#endif
  return num;
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
static INLINE void decodeUInt16LE(const byte *buffer,
                                  size_t     *offset,
                                  uint16_t   *decoded)
{
  *decoded = getUInt16LE(buffer + *offset);
  *offset += sizeof(uint16_t);
}

/**
 * Store a 16 bit number in a buffer in little-endian representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static INLINE void storeUInt16LE(byte* data, uint16_t num)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = bswap_16(num);
#endif
  PUT_UNALIGNED(uint16_t, data, num);
}

/**
 * Encode a 16 bit unsigned number into a buffer at a given offset
 * using a little-endian representation. The offset will be advanced
 * to first byte after the encoded number.
 *
 * @param data     The buffer to encode into
 * @param offset   A pointer to the offset at which to start encoding
 * @param toEncode The number to encode
 **/
static INLINE void encodeUInt16LE(byte     *data,
                                  size_t   *offset,
                                  uint16_t  toEncode)
{
  storeUInt16LE(data + *offset, toEncode);
  *offset += sizeof(uint16_t);
}

/**
 * Special function wrapper required for compile-time assertions. This
 * function will fail to compile if any of the uint*_t types are not of the
 * size we expect. This function should never be called.
 **/
void numericCompileTimeAssertions(void);

#endif /* NUMERIC_H */
