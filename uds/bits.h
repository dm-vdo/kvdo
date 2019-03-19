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
 * $Id: //eng/uds-releases/gloria/src/uds/bits.h#1 $
 */

#ifndef BITS_H
#define BITS_H 1

#include "compiler.h"
#include "numeric.h"
#include "typeDefs.h"

/*
 * These bit stream and bit field utility routines are used for the
 * non-byte aligned delta indices.
 *
 * Bits and bytes are numbered in little endian order.  For example: Within
 * a byte, bit 0 is the least significant bit (0x1), and bit 7 is the most
 * significant bit (0x80).  Within a bit stream, bit 7 is the most
 * signficant bit of byte 0, and bit 8 is the least significant bit of byte
 * 1.  Within a byte array, a byte's number corresponds to it's index in
 * the array.
 *
 * The implementation assumes that the native machine is little endian, and
 * that performance is very important.  These assumptions match our current
 * operating environment.
 */

/**
 * This is the largest field size supported by getField & setField.  Any
 * field that is larger is not guaranteed to fit in a single, byte aligned
 * uint32_t.
 **/
enum { MAX_FIELD_BITS = (sizeof(uint32_t) - 1) * CHAR_BIT + 1 };

/**
 * This is the number of guard bytes needed at the end of the memory byte
 * array when using the bit utilities.  3 bytes are needed when getField &
 * setField access a field, because they will access some "extra" bytes
 * past the end of the field.  And 7 bytes are needed when getBigField &
 * setBigField access a big field, for the same reason.  Note that moveBits
 * calls getBigField & setBigField.  7 is rewritten to make it clear how it
 * is derived.
 **/
enum { POST_FIELD_GUARD_BYTES = sizeof(uint64_t) - 1 };

/**
 * Get a bit field from a bit stream
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 *
 * @return the bit field
 **/
static INLINE unsigned int getField(const byte *memory, uint64_t offset,
                                    int size)
{
  const void *addr = memory + offset / CHAR_BIT;
  return (getUInt32LE(addr) >> (offset % CHAR_BIT)) & ((1 << size) - 1);
}

/**
 * Set a bit field in a bit stream
 *
 * @param value   The value to put into the field
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 *
 * @return the bit field
 **/
static INLINE void setField(unsigned int value, byte *memory, uint64_t offset,
                            int size)
{
  void *addr = memory + offset / CHAR_BIT;
  int shift = offset % CHAR_BIT;
  uint32_t data = getUInt32LE(addr);
  data &= ~(((1 << size) - 1) << shift);
  data |= value << shift;
  storeUInt32LE(addr, data);
}

/**
 * Set a bit field in a bit stream to all ones
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 *
 * @return the bit field
 **/
static INLINE void setOne(byte *memory, uint64_t offset, int size)
{
  if (size > 0) {
    byte *addr = memory + offset / CHAR_BIT;
    int shift = offset % CHAR_BIT;
    int count = size + shift > CHAR_BIT ? CHAR_BIT - shift : size;
    *addr++ |= ((1 << count) - 1) << shift;
    for (size -= count; size > CHAR_BIT; size -= CHAR_BIT) {
      *addr++ = 0xFF;
    }
    if (size) {
      *addr |= ~(0xFF << size);
    }
  }
}

/**
 * Set a bit field in a bit stream to all zeros
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 *
 * @return the bit field
 **/
static INLINE void setZero(byte *memory, uint64_t offset, int size)
{
  if (size > 0) {
    byte *addr = memory + offset / CHAR_BIT;
    int shift = offset % CHAR_BIT;
    int count = size + shift > CHAR_BIT ? CHAR_BIT - shift : size;
    *addr++ &= ~(((1 << count) - 1) << shift);
    for (size -= count; size > CHAR_BIT; size -= CHAR_BIT) {
      *addr++ = 0;
    }
    if (size) {
      *addr &= 0xFF << size;
    }
  }
}

/**
 * Get a byte stream from a bit stream, reading a whole number of bytes
 * from an arbitrary bit boundary.
 *
 * @param memory       The base memory byte address for the bit stream
 * @param offset       The bit offset of the start of the bit stream
 * @param destination  Where to store the bytes
 * @param size         The number of bytes
 **/
void getBytes(const byte *memory, uint64_t offset, byte *destination, int size);

/**
 * Store a byte stream into a bit stream, writing a whole number of bytes
 * to an arbitrary bit boundary.
 *
 * @param memory  The base memory byte address for the bit stream
 * @param offset  The bit offset of the start of the bit stream
 * @param source  Where to read the bytes
 * @param size    The number of bytes
 **/
void setBytes(byte *memory, uint64_t offset, const byte *source, int size);

/**
 * Move bits from one field to another.  When the fields overlap, behave as
 * if we first move all the bits from the source to a temporary value, and
 * then move all the bits from the temporary value to the destination.
 *
 * @param sMemory         The base source memory byte address
 * @param source          Bit offset into memory for the source start
 * @param dMemory         The base destination memory byte address
 * @param destination     Bit offset into memory for the destination start
 * @param size            The number of bits in the field
 **/
void moveBits(const byte *sMemory, uint64_t source, byte *dMemory,
              uint64_t destination, int size);

/**
 * Compare bits from one field to another, testing for sameness
 *
 * @param mem1     The base memory byte address (first field)
 * @param offset1  Bit offset into the memory for the start (first field)
 * @param mem2     The base memory byte address (second field)
 * @param offset2  Bit offset into the memory for the start (second field)
 * @param size     The number of bits in the field
 *
 * @return true if fields are the same, false if different
 **/
bool sameBits(const byte *mem1, uint64_t offset1, const byte *mem2,
              uint64_t offset2, int size)
  __attribute__((warn_unused_result));

#endif /* BITS_H */
