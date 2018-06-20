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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/numeric.h#3 $
 */

#ifndef NUMERIC_H
#define NUMERIC_H 1

#include <asm/byteorder.h>
#include <linux/types.h>

#include "common.h" /* for "byte" */

// GCC normally defines these three macros (and PDP-endian which we ignore).
#if !defined(__ORDER_LITTLE_ENDIAN__) || !defined(__ORDER_BIG_ENDIAN__) \
  || !defined(__BYTE_ORDER__)
#error "GCC byte order macros not defined?"
#endif

#define bswap_16 __swab16

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
#define UNALIGNED_WRAPPER(TYPE) \
  unaligned_wrap_##TYPE
#define UNALIGNED_WRAPPER_DEF(TYPE)                                 \
  typedef struct __attribute__((packed, may_alias)) { TYPE value; } \
  UNALIGNED_WRAPPER(TYPE)
UNALIGNED_WRAPPER_DEF(uint64_t);
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
static inline int minInt(int a, int b)
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
static inline int maxInt(int a, int b)
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
static inline long maxLong(long a, long b)
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
static inline unsigned long maxULong(unsigned long a, unsigned long b)
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
static inline size_t minSizeT(size_t a, size_t b)
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
static inline size_t maxSizeT(size_t a, size_t b)
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
static inline unsigned int maxUInt(unsigned int a, unsigned int b)
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
static inline uint64_t minUInt64(uint64_t a, uint64_t b)
{
  return ((a < b) ? a : b);
}

/**
 * Find the maximum of two uint64_ts.
 *
 * @param a The first uint64_t
 * @param b The second uint64_t
 *
 * @return The greater of a and b
 **/
__attribute__((warn_unused_result))
static inline uint64_t maxUInt64(uint64_t a, uint64_t b)
{
  return ((a > b) ? a : b);
}

/**
 * Extract a 64 bit number from a buffer stored in
 * big-endian representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static inline uint64_t getUInt64BE(const byte* data)
{
  uint64_t num = GET_UNALIGNED(uint64_t, data);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  return num;
}

/**
 * Extract a 64 bit, big-endian number from a buffer at a specified offset.
 * The offset will be advanced to the first byte after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to extract
 * @param decoded A pointer to hold the extracted number
 **/
static inline void decodeUInt64BE(const byte *buffer,
                                  size_t     *offset,
                                  uint64_t   *decoded)
{
  *decoded = getUInt64BE(buffer + *offset);
  *offset += sizeof(uint64_t);
}

/**
 * Store a 64 bit number in a buffer in
 * big-endian representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static inline void storeUInt64BE(byte* data, uint64_t num)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  PUT_UNALIGNED(uint64_t, data, num);
}

/**
 * Encode a 64 bit number into a buffer at a given offset using a
 * big-endian representation. The offset will be advanced to first byte
 * after the encoded number.
 *
 * @param data     The buffer to encode into
 * @param offset   A pointer to the offset at which to start encoding
 * @param toEncode The number to encode
 **/
static inline void encodeUInt64BE(byte     *data,
                                  size_t   *offset,
                                  uint64_t  toEncode)
{
  storeUInt64BE(data + *offset, toEncode);
  *offset += sizeof(uint64_t);
}

/**
 * Extract a 32 bit number from a buffer stored in
 * big-endian representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static inline uint32_t getUInt32BE(const byte* data)
{
  uint32_t num = GET_UNALIGNED(uint32_t, data);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap32(num);
#endif
  return num;
}

/**
 * Extract a 32 bit, big-endian number from a buffer at a specified offset.
 * The offset will be advanced to the first byte after the number.
 *
 * @param buffer  The buffer from which to extract the number
 * @param offset  A pointer to the offset into the buffer at which to extract
 * @param decoded A pointer to hold the extracted number
 **/
static inline void decodeUInt32BE(const byte *buffer,
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
static inline void storeUInt32BE(byte* data, uint32_t num)
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
static inline void encodeUInt32BE(byte     *data,
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
static inline uint16_t getUInt16BE(const byte* data)
{
  uint16_t num = GET_UNALIGNED(uint16_t, data);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // gcc doesn't give us 16-bit swap for x86_64 but glibc does
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
static inline void decodeUInt16BE(const byte *buffer,
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
static inline void storeUInt16BE(byte* data, uint16_t num)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // gcc doesn't give us 16-bit swap for x86_64 but glibc does
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
static inline void encodeUInt16BE(byte     *data,
                                  size_t   *offset,
                                  uint16_t  toEncode)
{
  storeUInt16BE(data + *offset, toEncode);
  *offset += sizeof(uint16_t);
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
static inline uint64_t getUInt64LE(const byte* data)
{
  uint64_t num = GET_UNALIGNED(uint64_t, data);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  return num;
}

/**
 * Extract a 64-bit little-endian number from a buffer at a specified offset.
 * The offset will be advanced to the first byte after the number.
 *
 * @param buffer   The buffer from which to extract the number
 * @param offset   A pointer to the offset into the buffer at which to extract
 * @param decoded  A pointer to hold the extracted number
 **/
static inline void decodeUInt64LE(const byte *buffer,
                                  size_t     *offset,
                                  uint64_t   *decoded)
{
  *decoded = getUInt64LE(buffer + *offset);
  *offset += sizeof(uint64_t);
}

/**
 * Store a 64 bit number in a buffer in
 * little-endian representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static inline void storeUInt64LE(byte* data, uint64_t num)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap64(num);
#endif
  PUT_UNALIGNED(uint64_t, data, num);
}

/**
 * Encode a 64-bit number into a buffer at a given offset using a
 * little-endian representation. The offset will be advanced to first byte
 * after the encoded number.
 *
 * @param data      The buffer to encode into
 * @param offset    A pointer to the offset at which to start encoding
 * @param toEncode  The number to encode
 **/
static inline void encodeUInt64LE(byte     *data,
                                  size_t   *offset,
                                  uint64_t  toEncode)
{
  storeUInt64LE(data + *offset, toEncode);
  *offset += sizeof(uint64_t);
}

/**
 * Extract a 32 bit number from a buffer stored in
 * little-endian representation.
 *
 * @param data The buffer from which to extract the number
 *
 * @return The extracted quantity
 **/
__attribute__((warn_unused_result))
static inline uint32_t getUInt32LE(const byte* data)
{
  uint32_t num = GET_UNALIGNED(uint32_t, data);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap32(num);
#endif
  return num;
}

/**
 * Store a 32 bit number in a buffer in
 * little-endian representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static inline void storeUInt32LE(byte* data, uint32_t num)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap32(num);
#endif
  PUT_UNALIGNED(uint32_t, data, num);
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
static inline uint16_t getUInt16LE(const byte* data)
{
  uint16_t num = GET_UNALIGNED(uint16_t, data);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap16(num);
#endif
  return num;
}

/**
 * Store a 16 bit number in a buffer in
 * little-endian representation.
 *
 * @param data The buffer in which to store the number
 * @param num  The number to store
 **/
static inline void storeUInt16LE(byte* data, uint16_t num)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
  num = __builtin_bswap16(num);
#endif
  PUT_UNALIGNED(uint16_t, data, num);
}

/**
 * Special function wrapper required for compile-time assertions. This
 * function will fail to compile if any of the uint*_t types are not of the
 * size we expect. This function should never be called.
 **/
void numericCompileTimeAssertions(void);

#endif /* NUMERIC_H */
