/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/fixedLayout.c#1 $
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

static const Header *CURRENT_LAYOUT_HEADER = &LAYOUT_HEADER_3_0;

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

/**********************************************************************/
int encodeFixedLayout(const FixedLayout *layout, Buffer *buffer)
{
  STATIC_ASSERT_SIZEOF(PartitionID, sizeof(byte));
  Header header = *CURRENT_LAYOUT_HEADER;
  header.size = getEncodedSize(layout);

  if (!ensureAvailableSpace(buffer, getFixedLayoutEncodedSize(layout))) {
    return UDS_BUFFER_ERROR;
  }

  int result = encodeHeader(&header, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  Layout3_0 layoutHeader = {
    .firstFree      = layout->firstFree,
    .lastFree       = layout->lastFree,
    .partitionCount = (byte) layout->numPartitions,
  };
  result = putBytes(buffer, sizeof(Layout3_0), &layoutHeader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  for (Partition *partition = layout->head; partition != NULL;
       partition = partition->next) {
    Partition3_0 partitionState = {
      .id     = partition->id,
      .offset = partition->offset,
      .base   = partition->base,
      .count  = partition->count,
    };
    result = putBytes(buffer, sizeof(Partition3_0), &partitionState);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return VDO_SUCCESS;
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
  result = validateHeader(CURRENT_LAYOUT_HEADER, &header, false, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  Layout3_0 layoutHeader;
  result = getBytesFromBuffer(buffer, sizeof(Layout3_0), &layoutHeader);
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

  for (size_t i = 0; i < layout->numPartitions; i++) {
    Partition3_0 partitionHeader;
    result = getBytesFromBuffer(buffer, sizeof(Partition3_0),
                                &partitionHeader);
    if (result != UDS_SUCCESS) {
      break;
    }

    result = allocatePartition(layout, partitionHeader.id,
                               partitionHeader.offset, partitionHeader.base,
                               partitionHeader.count);
    if (result != VDO_SUCCESS) {
      break;
    }
  }

  if (result != VDO_SUCCESS) {
    freeFixedLayout(&layout);
    return result;
  }

  *layoutPtr = layout;
  return VDO_SUCCESS;
}
