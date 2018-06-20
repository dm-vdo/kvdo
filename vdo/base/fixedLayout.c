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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/fixedLayout.c#2 $
 */

#include "fixedLayout.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"

#include "header.h"
#include "statusCodes.h"

const BlockCount ALL_FREE_BLOCKS = (uint64_t) -1;

struct fixedLayout {
  PhysicalBlockNumber  firstFree;
  PhysicalBlockNumber  lastFree;
  size_t               numPartitions;
  Partition           *head;
};

struct partition {
  PartitionID          id;     // The id of this partition
  FixedLayout         *layout; // The layout to which this partition belongs
  PhysicalBlockNumber  offset; // The offset into the layout of this partition
  PhysicalBlockNumber  base;   // The untranslated number of the first block
  BlockCount           count;  // The number of blocks in the partition
  Partition           *next;   // A pointer to the next partition in the layout
};

typedef struct {
  PhysicalBlockNumber firstFree;
  PhysicalBlockNumber lastFree;
  byte                partitionCount;
} __attribute__((packed)) Layout3_0;

typedef struct {
  PartitionID         id;
  PhysicalBlockNumber offset;
  PhysicalBlockNumber base;
  BlockCount          count;
} __attribute__((packed)) Partition3_0;

static const Header LAYOUT_HEADER_3_0 = {
  .id = FIXED_LAYOUT,
  .version = {
    .majorVersion = 3,
    .minorVersion = 0,
  },
  .size = sizeof(Layout3_0),   // Minimum size (contains no partitions)
};

/**********************************************************************/
int makeFixedLayout(BlockCount            totalBlocks,
                    PhysicalBlockNumber   startOffset,
                    FixedLayout         **layoutPtr)
{
  FixedLayout *layout;
  int result = ALLOCATE(1, FixedLayout, "fixed layout", &layout);
  if (result != UDS_SUCCESS) {
    return result;
  }

  layout->firstFree     = startOffset;
  layout->lastFree      = startOffset + totalBlocks;
  layout->numPartitions = 0;
  layout->head          = NULL;

  *layoutPtr = layout;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeFixedLayout(FixedLayout **layoutPtr)
{
  FixedLayout *layout = *layoutPtr;
  if (layout == NULL) {
    return;
  }

  while (layout->head != NULL) {
    Partition *part = layout->head;
    layout->head = part->next;
    FREE(part);
  }

  FREE(layout);
  *layoutPtr = NULL;
}

/**********************************************************************/
BlockCount getTotalFixedLayoutSize(const FixedLayout *layout)
{
  BlockCount size = getFixedLayoutBlocksAvailable(layout);
  for (Partition *partition = layout->head; partition != NULL;
       partition = partition->next) {
    size += partition->count;
  }

  return size;
}

/**********************************************************************/
int getPartition(FixedLayout *layout, PartitionID id, Partition **partitionPtr)
{
  for (Partition *partition = layout->head; partition != NULL;
       partition = partition->next) {
    if (partition->id == id) {
      if (partitionPtr != NULL) {
        *partitionPtr = partition;
      }
      return VDO_SUCCESS;
    }
  }

  return VDO_UNKNOWN_PARTITION;
}

/**********************************************************************/
int translateToPBN(const Partition     *partition,
                   PhysicalBlockNumber  partitionBlockNumber,
                   PhysicalBlockNumber *layerBlockNumber)
{
  if (partition == NULL) {
    *layerBlockNumber = partitionBlockNumber;
    return VDO_SUCCESS;
  }

  if (partitionBlockNumber < partition->base) {
    return VDO_OUT_OF_RANGE;
  }

  PhysicalBlockNumber offsetFromBase = partitionBlockNumber - partition->base;
  if (offsetFromBase >= partition->count) {
    return VDO_OUT_OF_RANGE;
  }

  *layerBlockNumber = partition->offset + offsetFromBase;
  return VDO_SUCCESS;
}

/**********************************************************************/
int translateFromPBN(const Partition     *partition,
                     PhysicalBlockNumber  layerBlockNumber,
                     PhysicalBlockNumber *partitionBlockNumberPtr)
{
  if (partition == NULL) {
    *partitionBlockNumberPtr = layerBlockNumber;
    return VDO_SUCCESS;
  }

  if (layerBlockNumber < partition->offset) {
    return VDO_OUT_OF_RANGE;
  }

  PhysicalBlockNumber partitionBlockNumber
    = layerBlockNumber - partition->offset;
  if (partitionBlockNumber >= partition->count) {
    return VDO_OUT_OF_RANGE;
  }

  *partitionBlockNumberPtr = partitionBlockNumber + partition->base;
  return VDO_SUCCESS;
}

/**********************************************************************/
BlockCount getFixedLayoutBlocksAvailable(const FixedLayout *layout)
{
  return layout->lastFree - layout->firstFree;
}

/**
 * Allocate a partition. The partition will be attached to the partition
 * list in the layout.
 *
 * @param layout     The layout containing the partition
 * @param id         The id of the partition
 * @param offset     The offset into the layout at which the partition begins
 * @param base       The number of the first block for users of the partition
 * @param blockCount The number of blocks in the partition
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocatePartition(FixedLayout         *layout,
                             byte                 id,
                             PhysicalBlockNumber  offset,
                             PhysicalBlockNumber  base,
                             BlockCount           blockCount)
{
  Partition *partition;
  int result = ALLOCATE(1, Partition, "fixed layout partition", &partition);
  if (result != UDS_SUCCESS) {
    return result;
  }

  partition->id                  = id;
  partition->layout              = layout;
  partition->offset              = offset;
  partition->base                = base;
  partition->count               = blockCount;
  partition->next                = layout->head;
  layout->head                   = partition;

  return VDO_SUCCESS;
}

/**********************************************************************/
int makeFixedLayoutPartition(FixedLayout         *layout,
                             PartitionID          id,
                             BlockCount           blockCount,
                             PartitionDirection   direction,
                             PhysicalBlockNumber  base)
{
  BlockCount freeBlocks = layout->lastFree - layout->firstFree;
  if (blockCount == ALL_FREE_BLOCKS) {
    if (freeBlocks == 0) {
      return VDO_NO_SPACE;
    } else {
      blockCount = freeBlocks;
    }
  } else if (blockCount > freeBlocks) {
    return VDO_NO_SPACE;
  }

  int result = getPartition(layout, id, NULL);
  if (result != VDO_UNKNOWN_PARTITION) {
    return VDO_PARTITION_EXISTS;
  }

  PhysicalBlockNumber offset = ((direction == FROM_END)
                                ? (layout->lastFree - blockCount)
                                : layout->firstFree);
  result = allocatePartition(layout, id, offset, base, blockCount);
  if (result != VDO_SUCCESS) {
    return result;
  }

  layout->numPartitions++;
  if (direction == FROM_END) {
    layout->lastFree = layout->lastFree - blockCount;
  } else {
    layout->firstFree += blockCount;
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
BlockCount getFixedLayoutPartitionSize(const Partition *partition)
{
  return partition->count;
}

/**********************************************************************/
PhysicalBlockNumber getFixedLayoutPartitionOffset(const Partition *partition)
{
  return partition->offset;
}

/**********************************************************************/
PhysicalBlockNumber getFixedLayoutPartitionBase(const Partition *partition)
{
  return partition->base;
}

/**********************************************************************/
static inline size_t getEncodedSize(const FixedLayout *layout)
{
  return sizeof(Layout3_0) + (sizeof(Partition3_0) * layout->numPartitions);
}

/**********************************************************************/
size_t getFixedLayoutEncodedSize(const FixedLayout *layout)
{
  return ENCODED_HEADER_SIZE + getEncodedSize(layout);
}

/**
 * Encode a null-terminated list of fixed layout partitions into a buffer
 * using partition format 3.0.
 *
 * @param layout  The layout containing the list of partitions to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int encodePartitions_3_0(const FixedLayout *layout, Buffer *buffer)
{
  for (const Partition *partition = layout->head;
       partition != NULL;
       partition = partition->next) {
    STATIC_ASSERT_SIZEOF(PartitionID, sizeof(byte));
    int result = putByte(buffer, partition->id);
    if (result != UDS_SUCCESS) {
      return result;
    }

    result = putUInt64LEIntoBuffer(buffer, partition->offset);
    if (result != UDS_SUCCESS) {
      return result;
    }

    result = putUInt64LEIntoBuffer(buffer, partition->base);
    if (result != UDS_SUCCESS) {
      return result;
    }

    result = putUInt64LEIntoBuffer(buffer, partition->count);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}

/**
 * Encode the header fields of a fixed layout into a buffer using layout
 * format 3.0.
 *
 * @param layout  The layout to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int encodeLayout_3_0(const FixedLayout *layout, Buffer *buffer)
{
  int result = ASSERT(layout->numPartitions <= UINT8_MAX,
                      "fixed layout partition count must fit in a byte");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, layout->firstFree);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, layout->lastFree);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return putByte(buffer, layout->numPartitions);
}

/**********************************************************************/
int encodeFixedLayout(const FixedLayout *layout, Buffer *buffer)
{
  if (!ensureAvailableSpace(buffer, getFixedLayoutEncodedSize(layout))) {
    return UDS_BUFFER_ERROR;
  }

  Header header = LAYOUT_HEADER_3_0;
  header.size = getEncodedSize(layout);
  int result = encodeHeader(&header, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t initialLength = contentLength(buffer);

  result = encodeLayout_3_0(layout, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t encodedSize = contentLength(buffer) - initialLength;
  result = ASSERT(encodedSize == sizeof(Layout3_0),
                "encoded size of fixed layout header must match structure");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = encodePartitions_3_0(layout, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  encodedSize = contentLength(buffer) - initialLength;
  return ASSERT(encodedSize == header.size,
                "encoded size of fixed layout must match header size");
}

/**
 * Decode a sequence of fixed layout partitions from a buffer
 * using partition format 3.0.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param layout  The layout in which to allocate the decoded partitions
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodePartitions_3_0(Buffer *buffer, FixedLayout *layout)
{
  for (size_t i = 0; i < layout->numPartitions; i++) {
    byte id;
    int result = getByte(buffer, &id);
    if (result != UDS_SUCCESS) {
      return result;
    }

    uint64_t offset;
    result = getUInt64LEFromBuffer(buffer, &offset);
    if (result != UDS_SUCCESS) {
      return result;
    }

    uint64_t base;
    result = getUInt64LEFromBuffer(buffer, &base);
    if (result != UDS_SUCCESS) {
      return result;
    }

    uint64_t count;
    result = getUInt64LEFromBuffer(buffer, &count);
    if (result != UDS_SUCCESS) {
      return result;
    }

    result = allocatePartition(layout, id, offset, base, count);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}

/**
 * Decode the header fields of a fixed layout from a buffer using layout
 * format 3.0.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param layout  The structure to receive the decoded fields
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodeLayout_3_0(Buffer *buffer, Layout3_0 *layout)
{
  size_t initialLength = contentLength(buffer);

  int result = getUInt64LEFromBuffer(buffer, &layout->firstFree);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &layout->lastFree);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getByte(buffer, &layout->partitionCount);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t decodedSize = initialLength - contentLength(buffer);
  return ASSERT(decodedSize == sizeof(Layout3_0),
                "decoded size of fixed layout header must match structure");
}

/**********************************************************************/
int decodeFixedLayout(Buffer *buffer, FixedLayout **layoutPtr)
{
  Header header;
  int result = decodeHeader(buffer, &header);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Layout is variable size, so only do a minimum size check here.
  result = validateHeader(&LAYOUT_HEADER_3_0, &header, false, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  Layout3_0 layoutHeader;
  result = decodeLayout_3_0(buffer, &layoutHeader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (contentLength(buffer)
      < (sizeof(Partition3_0) * layoutHeader.partitionCount)) {
    return VDO_UNSUPPORTED_VERSION;
  }

  FixedLayout *layout;
  result = ALLOCATE(1, FixedLayout, "fixed layout", &layout);
  if (result != UDS_SUCCESS) {
    return result;
  }

  layout->firstFree     = layoutHeader.firstFree;
  layout->lastFree      = layoutHeader.lastFree;
  layout->numPartitions = layoutHeader.partitionCount;

  result = decodePartitions_3_0(buffer, layout);
  if (result != VDO_SUCCESS) {
    freeFixedLayout(&layout);
    return result;
  }

  *layoutPtr = layout;
  return VDO_SUCCESS;
}
