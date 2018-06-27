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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/compressedBlock.h#3 $
 */

#ifndef COMPRESSED_BLOCK_H
#define COMPRESSED_BLOCK_H

#include "blockMappingState.h"
#include "header.h"

/**
 * The header of a compressed block.
 **/
typedef union __attribute__((packed)) {
  struct __attribute__((packed)) {
    /** Unsigned 32-bit major and minor versions, in little-endian byte order */
    PackedVersionNumber version;

    /** List of unsigned 16-bit compressed block sizes, in little-endian order */
    byte sizes[MAX_COMPRESSION_SLOTS][2];
  } fields;

  // A raw view of the packed encoding.
  byte raw[4 + 4 + (2 * MAX_COMPRESSION_SLOTS)];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // This view is only valid on little-endian machines and is only present for
  // ease of directly examining compressed block headers in GDB.
  struct __attribute__((packed)) {
    VersionNumber version;
    uint16_t      sizes[MAX_COMPRESSION_SLOTS];
  } littleEndian;
#endif
} CompressedBlockHeader;

/**
 * The compressed block overlay.
 **/
typedef struct {
  CompressedBlockHeader header;
  char                  data[];
} __attribute__((packed)) CompressedBlock;

/**
 * Initializes/resets a compressed block header.
 *
 * @param header        the header
 *
 * When done, the version number is set to the current version, and all
 * fragments are empty.
 **/
void resetCompressedBlockHeader(CompressedBlockHeader *header);

/**
 * Get a reference to a compressed fragment from a compression block.
 *
 * @param [in]  mappingState    the mapping state for the look up
 * @param [in]  buffer          buffer that contains compressed data
 * @param [in]  blockSize       size of a data block
 * @param [out] fragmentOffset  the offset of the fragment within a
 *                              compressed block
 * @param [out] fragmentSize    the size of the fragment
 *
 * @return If a valid compressed fragment is found, VDO_SUCCESS;
 *         otherwise, VDO_INVALID_FRAGMENT if the fragment is invalid.
 **/
int getCompressedBlockFragment(BlockMappingState  mappingState,
                               char              *buffer,
                               BlockSize          blockSize,
                               uint16_t          *fragmentOffset,
                               uint16_t          *fragmentSize);

/**
 * Copy a fragment into the compressed block.
 *
 * @param block      the compressed block
 * @param fragment   the number of the fragment
 * @param offset     the byte offset of the fragment in the data area
 * @param data       a pointer to the compressed data
 * @param size       the size of the data
 *
 * @note no bounds checking -- the data better fit without smashing other stuff
 **/
void putCompressedBlockFragment(CompressedBlock *block,
                                unsigned int     fragment,
                                uint16_t         offset,
                                const char      *data,
                                uint16_t         size);

#endif // COMPRESSED_BLOCK_H
