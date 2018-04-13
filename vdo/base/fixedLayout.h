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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/fixedLayout.h#1 $
 */

#ifndef FIXED_LAYOUT_H
#define FIXED_LAYOUT_H

#include "buffer.h"

#include "types.h"

typedef enum {
  FROM_BEGINNING,
  FROM_END,
} PartitionDirection;

extern const BlockCount ALL_FREE_BLOCKS;

/**
 * A fixed layout is like a traditional disk partitioning scheme.  In the
 * beginning there is one large unused area, of which parts are carved off.
 * Each carved off section has its own internal offset and size.
 **/
typedef struct fixedLayout FixedLayout;
typedef struct partition Partition;

/**
 * Make an unpartitioned fixed layout.
 *
 * @param [in]  totalBlocks  The total size of the layout, in blocks
 * @param [in]  startOffset  The block offset in the underlying layer at which
 *                           the fixed layout begins
 * @param [out] layoutPtr    The pointer to hold the resulting layout
 *
 * @return a success or error code
 **/
int makeFixedLayout(BlockCount            totalBlocks,
                    PhysicalBlockNumber   startOffset,
                    FixedLayout         **layoutPtr)
  __attribute__((warn_unused_result));

/**
 * Free the fixed layout and null out the reference to it.
 *
 * @param layoutPtr  The reference to the layout to free
 *
 * @note all partitions created by this layout become invalid pointers
 **/
void freeFixedLayout(FixedLayout **layoutPtr);

/**
 * Get the total size of the layout in blocks.
 *
 * @param layout  The layout
 *
 * @return The size of the layout
 **/
BlockCount getTotalFixedLayoutSize(const FixedLayout *layout)
  __attribute__((warn_unused_result));

/**
 * Get a partition by id.
 *
 * @param layout        The layout from which to get a partition
 * @param id            The id of the partition
 * @param partitionPtr  A pointer to hold the partition
 *
 * @return VDO_SUCCESS or an error
 **/
int getPartition(FixedLayout *layout, PartitionID id, Partition **partitionPtr)
  __attribute__((warn_unused_result));

/**
 * Translate a block number from the partition's view to the layer's
 *
 * @param partition             The partition to use for translation
 * @param partitionBlockNumber  The block number relative to the partition
 * @param layerBlockNumber      The block number relative to the layer
 *
 * @return  VDO_SUCCESS or an error code
 **/
int translateToPBN(const Partition     *partition,
                   PhysicalBlockNumber  partitionBlockNumber,
                   PhysicalBlockNumber *layerBlockNumber)
  __attribute__((warn_unused_result));

/**
 * Translate a block number from the layer's view to the partition's.
 * This is the inverse of translateToPBN().
 *
 * @param partition             The partition to use for translation
 * @param layerBlockNumber      The block number relative to the layer
 * @param partitionBlockNumber  The block number relative to the partition
 *
 * @return  VDO_SUCCESS or an error code
 **/
int translateFromPBN(const Partition     *partition,
                     PhysicalBlockNumber  layerBlockNumber,
                     PhysicalBlockNumber *partitionBlockNumber)
  __attribute__((warn_unused_result));

/**
 * Return the number of unallocated blocks available.
 *
 * @param layout        the fixed layout
 *
 * @return the number of blocks yet unallocated to partitions
 **/
BlockCount getFixedLayoutBlocksAvailable(const FixedLayout *layout)
  __attribute__((warn_unused_result));

/**
 * Create a new partition from the beginning or end of the unused space
 * within a fixed layout.
 *
 * @param   layout          the fixed layout
 * @param   id              the id of the partition to make
 * @param   blockCount      the number of blocks to carve out, if set
 *                          to ALL_FREE_BLOCKS, all remaining blocks will
 *                          be used
 * @param   direction       whether to carve out from beginning or end
 * @param   base            the number of the first block in the partition
 *                          from the point of view of its users
 *
 * @return a success or error code, particularly
 *      VDO_NO_SPACE if there are less than blockCount blocks remaining
 **/
int makeFixedLayoutPartition(FixedLayout         *layout,
                             PartitionID          id,
                             BlockCount           blockCount,
                             PartitionDirection   direction,
                             PhysicalBlockNumber  base)
  __attribute__((warn_unused_result));

/**
 * Return the size in blocks of a partition.
 *
 * @param partition       a partition of the fixedLayout
 *
 * @return the size of the partition in blocks
 **/
BlockCount getFixedLayoutPartitionSize(const Partition *partition)
  __attribute__((warn_unused_result));

/**
 * Get the first block of the partition in the layout.
 *
 * @param partition       a partition of the fixedLayout
 *
 * @return the partition's offset in blocks
 **/
PhysicalBlockNumber getFixedLayoutPartitionOffset(const Partition *partition)
  __attribute__((warn_unused_result));

/**
 * Get the number of the first block in the partition from the partition users
 * point of view.
 *
 * @param partition a partition of the fixedLayout
 *
 * @return the number of the first block in the partition
 **/
PhysicalBlockNumber getFixedLayoutPartitionBase(const Partition *partition)
  __attribute__((warn_unused_result));

/**
 * Get the size of an encoded layout
 *
 * @param layout The layout
 *
 * @return The encoded size of the layout
 **/
size_t getFixedLayoutEncodedSize(const FixedLayout *layout)
  __attribute__((warn_unused_result));

/**
 * Encode a layout into a buffer.
 *
 * @param layout The layout to encode
 * @param buffer The buffer to encode into
 *
 * @return UDS_SUCCESS or an error
 **/
int encodeFixedLayout(const FixedLayout *layout, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Decode a fixed layout from a buffer.
 *
 * @param [in]  buffer    The buffer from which to decode
 * @param [out] layoutPtr A pointer to hold the layout
 *
 * @return VDO_SUCCESS or an error
 **/
int decodeFixedLayout(Buffer *buffer, FixedLayout **layoutPtr)
  __attribute__((warn_unused_result));

#endif // FIXED_LAYOUT_H
