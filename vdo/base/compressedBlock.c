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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/base/compressedBlock.c#1 $
 */

#include "compressedBlock.h"

#include "memoryAlloc.h"

static const VersionNumber COMPRESSED_BLOCK_1_0 = {
  .majorVersion = 1,
  .minorVersion = 0,
};

static const VersionNumber *CURRENT_BLOCK_VERSION = &COMPRESSED_BLOCK_1_0;

/**********************************************************************/
void resetCompressedBlockHeader(CompressedBlockHeader *header)
{
  memset(header, 0, sizeof(*header));
  header->version = *CURRENT_BLOCK_VERSION;
}

/**********************************************************************/
int getCompressedBlockFragment(BlockMappingState  mappingState,
                               char              *buffer,
                               BlockSize          blockSize,
                               uint16_t          *fragmentOffset,
                               uint16_t          *fragmentSize)
{
  if (!isCompressed(mappingState)) {
    return VDO_INVALID_FRAGMENT;
  }

  CompressedBlockHeader *header = (CompressedBlockHeader *) buffer;
  if (!areSameVersion(&header->version, CURRENT_BLOCK_VERSION)) {
    return VDO_INVALID_FRAGMENT;
  }

  byte slot = getSlotFromState(mappingState);
  if (slot >= MAX_COMPRESSION_SLOTS) {
    return VDO_INVALID_FRAGMENT;
  }

  uint16_t compressedSize = header->sizes[slot];
  uint16_t offset         = sizeof(CompressedBlockHeader);
  for (unsigned int i = 0; i < slot; i++) {
    offset += header->sizes[i];
    if (offset >= blockSize) {
      return VDO_INVALID_FRAGMENT;
    }
  }

  if ((offset + compressedSize) > blockSize) {
    return VDO_INVALID_FRAGMENT;
  }

  *fragmentOffset = offset;
  *fragmentSize   = compressedSize;
  return VDO_SUCCESS;
}

/**********************************************************************/
void putCompressedBlockFragment(CompressedBlock *block,
                                unsigned int     fragment,
                                uint16_t         offset,
                                const char      *data,
                                uint16_t         size)
{
  block->header.sizes[fragment] = size;
  memcpy(&block->data[offset], data, size);
}
