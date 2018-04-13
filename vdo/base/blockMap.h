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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMap.h#1 $
 */

#ifndef BLOCK_MAP_H
#define BLOCK_MAP_H

#include "blockMapEntry.h"
#include "completion.h"
#include "fixedLayout.h"
#include "readOnlyModeContext.h"
#include "statistics.h"
#include "types.h"

/**
 * Compute the size in blocks required for a block map with the specified
 * parameters.
 *
 * @param totalEntries       The number of block map entries
 * @param slabSize           The size of a slab in blocks
 * @param dataBlocksPerSlab  The number of blocks per slab usable for data
 *
 * @return The number of blocks required
 *
 * @note The computation assumes that the page size is an integral multiple
 *       of the block size.
 **/
BlockCount computeBlockMapSize(BlockCount totalEntries,
                               BlockCount slabSize,
                               BlockCount dataBlocksPerSlab);

/**
 * Create a block map.
 *
 * @param [in]  logicalBlocks    The number of logical blocks for the VDO
 * @param [in]  threadConfig     The thread configuration of the VDO
 * @param [in]  flatPageCount    The number of flat pages
 * @param [in]  rootOrigin       The absolute PBN of the first root page
 * @param [in]  rootCount        The number of tree roots
 * @param [out] mapPtr           The pointer to hold the new block map
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeBlockMap(BlockCount           logicalBlocks,
                 const ThreadConfig  *threadConfig,
                 BlockCount           flatPageCount,
                 PhysicalBlockNumber  rootOrigin,
                 BlockCount           rootCount,
                 BlockMap           **mapPtr)
  __attribute__((warn_unused_result));

/**
 * Prepare to grow the block map by allocating an expanded collection of trees.
 *
 * @param map               The block map to grow
 * @param newLogicalBlocks  The new logical size of the VDO
 *
 * @return VDO_SUCCESS or an error
 **/
int prepareToGrowBlockMap(BlockMap *map, BlockCount newLogicalBlocks)
  __attribute__((warn_unused_result));

/**
 * Get the logical size to which this block map is prepared to grow.
 *
 * @param map  The block map
 *
 * @return The new number of entries the block map will be grown to or 0 if
 *         the block map is not prepared to grow
 **/
BlockCount getNewEntryCount(BlockMap *map)
  __attribute__((warn_unused_result));

/**
 * Grow a block map on which prepareToGrowBlockMap() has already been called.
 *
 * @param map         The block map to grow
 * @param completion  The object to notify when the growth is complete
 **/
void growBlockMap(BlockMap *map, VDOCompletion *completion);

/**
 * Abandon any preparations which were made to grow this block map.
 *
 * @param map  The map which won't be grown
 **/
void abandonBlockMapGrowth(BlockMap *map);

/**
 * Decode the state of a block map saved in a buffer, without creating page
 * caches.
 *
 * @param [in]  buffer         A buffer containing the super block state
 * @param [in]  logicalBlocks  The number of logical blocks for the VDO
 * @param [in]  threadConfig   The thread configuration of the VDO
 * @param [out] mapPtr         The pointer to hold the new block map
 *
 * @return VDO_SUCCESS or an error code
 **/
int decodeBlockMap(Buffer              *buffer,
                   BlockCount           logicalBlocks,
                   const ThreadConfig  *threadConfig,
                   BlockMap           **mapPtr)
  __attribute__((warn_unused_result));

/**
 * Create a block map from the saved state of a Sodium block map, and do any
 * necessary upgrade work.
 *
 * @param [in]  buffer         A buffer containing the super block state
 * @param [in]  logicalBlocks  The number of logical blocks for the VDO
 * @param [in]  threadConfig   The thread configuration of the VDO
 * @param [out] mapPtr         The pointer to hold the new block map
 *
 * @return VDO_SUCCESS or an error code
 **/
int decodeSodiumBlockMap(Buffer              *buffer,
                         BlockCount           logicalBlocks,
                         const ThreadConfig  *threadConfig,
                         BlockMap           **mapPtr)
  __attribute__((warn_unused_result));

/**
 * Allocate the page caches for a block map.
 *
 * @param map              The block map needing caches.
 * @param layer            The physical layer for the cache
 * @param readOnlyContext  The read only mode context
 * @param journal          The recovery journal (may be NULL)
 * @param nonce            The nonce to distinguish initialized pages
 * @param cacheSize        The block map cache size, in pages
 * @param maximumAge       The number of journal blocks before a dirtied page
 *                         is considered old and must be written out
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeBlockMapCaches(BlockMap            *map,
                       PhysicalLayer       *layer,
                       ReadOnlyModeContext *readOnlyContext,
                       RecoveryJournal     *journal,
                       Nonce                nonce,
                       PageCount            cacheSize,
                       BlockCount           maximumAge)
  __attribute__((warn_unused_result));

/**
 * Free a block map and null out the reference to it.
 *
 * @param mapPtr  A pointer to the block map to free
 **/
void freeBlockMap(BlockMap **mapPtr);

/**
 * Get the size of the encoded state of a block map.
 *
 * @return The encoded size of the map's state
 **/
size_t getBlockMapEncodedSize(void)
  __attribute__((warn_unused_result));

/**
 * Encode the state of a block map into a buffer.
 *
 * @param map     The block map to encode
 * @param buffer  The buffer to encode into
 *
 * @return UDS_SUCCESS or an error
 **/
int encodeBlockMap(const BlockMap *map, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Obtain any necessary state from the recovery journal that is needed for
 * normal block map operation.
 *
 * @param map      The map in question
 * @param journal  The journal to initialize from
 **/
void initializeBlockMapFromJournal(BlockMap *map, RecoveryJournal *journal);

/**
 * Get the portion of the block map for a given logical zone.
 *
 * @param map         The map
 * @param zoneNumber  The number of the zone
 *
 * @return The requested block map zone
 **/
BlockMapZone *getBlockMapZone(BlockMap *map, ZoneCount zoneNumber)
  __attribute__((warn_unused_result));

/**
 * Compute the logical zone on which the entry for a DataVIO
 * resides
 *
 * @param dataVIO  The DataVIO
 *
 * @return The logical zone number for the DataVIO
 **/
ZoneCount computeLogicalZone(DataVIO *dataVIO);

/**
 * Compute the block map slot in which the block map entry for a DataVIO
 * resides, and cache that number in the DataVIO.
 *
 * @param dataVIO  The DataVIO
 * @param callback The function to call once the slot has been found
 * @param threadID The thread on which to run the callback
 **/
void findBlockMapSlotAsync(DataVIO   *dataVIO,
                           VDOAction *callback,
                           ThreadID   threadID);

/**
 * Get number of block map pages at predetermined locations.
 *
 * @param map  The block map
 *
 * @return The number of fixed pages used by the map
 **/
PageCount getNumberOfFixedBlockMapPages(const BlockMap *map)
  __attribute__((warn_unused_result));

/**
 * Get number of block map entries.
 *
 * @param map  The block map
 *
 * @return The number of entries stored in the map
 **/
BlockCount getNumberOfBlockMapEntries(const BlockMap *map)
  __attribute__((warn_unused_result));

/**
 * Notify the block map that the recovery journal has finished a new block.
 * This method must be called from the journal zone thread.
 *
 * @param map                  The block map
 * @param recoveryBlockNumber  The sequence number of the finished recovery
 *                             journal block
 **/
void advanceBlockMapEra(BlockMap *map, SequenceNumber recoveryBlockNumber);

/**
 * Flush any dirty pages in the trees or page caches.
 *
 * @param map     The block map to flush
 * @param parent  The completion to notify when the block map is flushed
 **/
void flushBlockMap(BlockMap *map, VDOCompletion *parent);

/**
 * Mark the block map as closing so that all subsequent lookup requests will be
 * rejected. Asynchronously flush all dirty pages from all zones of the block
 * map.
 *
 *
 * @param map     The block map to close
 * @param parent  The completion to finish when the close is complete
 **/
void closeBlockMap(BlockMap *map, VDOCompletion *parent);

/**
 * Get the block number of the physical block containing the data for the
 * specified logical block number. All blocks are mapped to physical block
 * zero by default, which is conventionally the zero block.
 *
 * @param dataVIO  The DataVIO of the block to map
 **/
void getMappedBlockAsync(DataVIO *dataVIO);

/**
 * Associate the logical block number for a block represented by a DataVIO
 * with the physical block number in its newMapped field.
 *
 * @param dataVIO  The DataVIO of the block to map
 **/
void putMappedBlockAsync(DataVIO *dataVIO);

/**
 * Get the stats for the block map page cache.
 *
 * @param map  The block map containing the cache
 *
 * @return The block map statistics
 **/
BlockMapStatistics getBlockMapStatistics(BlockMap *map)
  __attribute__((warn_unused_result));

#endif // BLOCK_MAP_H
