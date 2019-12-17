/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/types.h#29 $
 */

#ifndef TYPES_H
#define TYPES_H

#include "blockMappingState.h"
#include "common.h"
#include "statusCodes.h"

/**
 * A size type in blocks.
 **/
typedef uint64_t BlockCount;

/**
 * The size of a block.
 **/
typedef uint16_t BlockSize;

/**
 * A count of compressed fragments
 **/
typedef uint8_t CompressedFragmentCount;

/**
 * A CRC-32 checksum
 **/
typedef uint32_t CRC32Checksum;

/**
 * A height within a tree.
 **/
typedef uint8_t Height;

/**
 * The logical block number as used by the consumer.
 **/
typedef uint64_t LogicalBlockNumber;

/**
 * The type of the nonce used to identify instances of VDO.
 **/
typedef uint64_t Nonce;

/**
 * A size in pages.
 **/
typedef uint32_t PageCount;

/**
 * A page number.
 **/
typedef uint32_t PageNumber;

/**
 * The size of a page.  Must be evenly divisible by block size.
 **/
typedef uint32_t PageSize;

/**
 * The physical (well, less logical) block number at which the block is found
 * on the underlying device.
 **/
typedef uint64_t PhysicalBlockNumber;

/**
 * A release version number. These numbers are used to make the numbering
 * space for component versions independent across release branches.
 *
 * Really an enum, but we have to specify the size for encoding; see
 * releaseVersions.h for the enumeration values.
 **/
typedef uint32_t ReleaseVersionNumber;

/**
 * A count of tree roots.
 **/
typedef uint8_t RootCount;

/**
 * A number of sectors.
 **/
typedef uint8_t SectorCount;

/**
 * A sequence number.
 **/
typedef uint64_t SequenceNumber;

/**
 * A size type in slabs.
 **/
typedef uint16_t SlabCount;

/**
 * A slot in a bin or block map page.
 **/
typedef uint16_t SlotNumber;

/**
 * A number of vios.
 **/
typedef uint16_t VIOCount;

/**
 * A VDO thread configuration.
 **/
typedef struct threadConfig ThreadConfig;

/**
 * A thread counter
 **/
typedef uint8_t ThreadCount;

/**
 * A thread ID
 *
 * Base-code threads are numbered sequentially starting from 0.
 **/
typedef uint8_t ThreadID;

/**
 * The thread ID returned when the current base code thread ID cannot be found
 * or is otherwise undefined.
 **/
static const ThreadID INVALID_THREAD_ID = (ThreadID) -1;

/**
 * A zone counter
 **/
typedef uint8_t ZoneCount;

/**
 * The type of request a vio is performing
 **/
typedef enum __attribute__((packed)) vioOperation {
  VIO_UNSPECIFIED_OPERATION = 0,
  VIO_READ                  = 1,
  VIO_WRITE                 = 2,
  VIO_READ_MODIFY_WRITE     = VIO_READ | VIO_WRITE,
  VIO_READ_WRITE_MASK       = VIO_READ_MODIFY_WRITE,
  VIO_FLUSH_BEFORE          = 4,
  VIO_FLUSH_AFTER           = 8,
} VIOOperation;

/**
 * vio types for statistics and instrumentation.
 **/
typedef enum __attribute__((packed)) {
  VIO_TYPE_UNINITIALIZED = 0,
  VIO_TYPE_DATA,
  VIO_TYPE_BLOCK_ALLOCATOR,
  VIO_TYPE_BLOCK_MAP,
  VIO_TYPE_BLOCK_MAP_INTERIOR,
  VIO_TYPE_COMPRESSED_BLOCK,
  VIO_TYPE_PARTITION_COPY,
  VIO_TYPE_RECOVERY_JOURNAL,
  VIO_TYPE_SLAB_JOURNAL,
  VIO_TYPE_SLAB_SUMMARY,
  VIO_TYPE_SUPER_BLOCK,
  VIO_TYPE_TEST,
} VIOType;

/**
 * The current operation on a physical block (from the point of view of the
 * recovery journal, slab journals, and reference counts.
 **/
typedef enum __attribute__((packed)) {
  DATA_DECREMENT      = 0,
  DATA_INCREMENT      = 1,
  BLOCK_MAP_DECREMENT = 2,
  BLOCK_MAP_INCREMENT = 3,
} JournalOperation;

/**
 * Partition IDs are encoded in the volume layout in the super block.
 **/
typedef enum __attribute__((packed)) {
  BLOCK_MAP_PARTITION        = 0,
  BLOCK_ALLOCATOR_PARTITION  = 1,
  RECOVERY_JOURNAL_PARTITION = 2,
  SLAB_SUMMARY_PARTITION     = 3,
} PartitionID;

/**
 * Check whether a VIOType is for servicing an external data request.
 *
 * @param vioType  The VIOType to check
 **/
static inline bool isDataVIOType(VIOType vioType)
{
  return (vioType == VIO_TYPE_DATA);
}

/**
 * Check whether a VIOType is for compressed block writes
 *
 * @param vioType  The VIOType to check
 **/
static inline bool isCompressedWriteVIOType(VIOType vioType)
{
  return (vioType == VIO_TYPE_COMPRESSED_BLOCK);
}

/**
 * Check whether a VIOType is for metadata
 *
 * @param vioType  The VIOType to check
 **/
static inline bool isMetadataVIOType(VIOType vioType)
{
  return ((vioType != VIO_TYPE_UNINITIALIZED)
          && !isDataVIOType(vioType)
          && !isCompressedWriteVIOType(vioType));
}

/**
 * Priority levels for asynchronous I/O operations performed on a vio.
 **/
typedef enum __attribute__((packed)) vioPriority {
  VIO_PRIORITY_LOW             = 0,
  VIO_PRIORITY_DATA            = VIO_PRIORITY_LOW,
  VIO_PRIORITY_COMPRESSED_DATA = VIO_PRIORITY_DATA,
  VIO_PRIORITY_METADATA,
  VIO_PRIORITY_HIGH,
} VIOPriority;

/**
 * Metadata types for the VDO.
 **/
typedef enum __attribute__((packed)) {
  VDO_METADATA_RECOVERY_JOURNAL = 1,
  VDO_METADATA_SLAB_JOURNAL,
} VDOMetadataType;

/**
 * The possible write policy values.
 **/
typedef enum {
  WRITE_POLICY_SYNC,    ///< All writes are synchronous, i. e., they
                        ///< are acknowledged only when the data is
                        ///< written to stable storage.
  WRITE_POLICY_ASYNC,   ///< Writes are acknowledged when the data is
                        ///< cached for writing to stable storage, subject
                        ///< to resiliency guarantees specified elsewhere.
  WRITE_POLICY_AUTO,    ///< The appropriate policy is chosen based on the
                        ///< underlying device
} WritePolicy;

typedef enum {
  ZONE_TYPE_ADMIN,
  ZONE_TYPE_JOURNAL,
  ZONE_TYPE_LOGICAL,
  ZONE_TYPE_PHYSICAL,
} ZoneType;

/**
 * A position in the block map where a block map entry is stored.
 **/
typedef struct {
  PhysicalBlockNumber pbn;
  SlotNumber          slot;
} BlockMapSlot;

/**
 * A position in the arboreal block map at a specific level.
 **/
typedef struct {
  PageNumber   pageIndex;
  BlockMapSlot blockMapSlot;
} BlockMapTreeSlot;

/**
 * The configuration of a single slab derived from the configured block size
 * and slab size.
 **/
typedef struct slabConfig {
  BlockCount slabBlocks;             ///< total number of blocks in the slab
  BlockCount dataBlocks;             ///< number of blocks available for data
  BlockCount referenceCountBlocks;   ///< number of blocks for refCounts
  BlockCount slabJournalBlocks;      ///< number of blocks for the slab journal
  /**
   * Number of blocks after which the slab journal starts pushing out a
   * ReferenceBlock for each new entry it receives.
   **/
  BlockCount slabJournalFlushingThreshold;
  /**
   * Number of blocks after which the slab journal pushes out all
   * ReferenceBlocks and makes all vios wait.
   **/
  BlockCount slabJournalBlockingThreshold;
  /**
   * Number of blocks after which the slab must be scrubbed before coming
   * online.
   **/
  BlockCount slabJournalScrubbingThreshold;
} __attribute__((packed)) SlabConfig;

/**
 * The configuration of the VDO service.
 **/
typedef struct vdoConfig {
  BlockCount    logicalBlocks;       ///< number of logical blocks
  BlockCount    physicalBlocks;      ///< number of physical blocks
  BlockCount    slabSize;            ///< number of blocks in a slab
  BlockCount    recoveryJournalSize; ///< number of recovery journal blocks
  BlockCount    slabJournalBlocks;   ///< number of slab journal blocks
} __attribute__((packed)) VDOConfig;

/**
 * The configuration parameters of the VDO service specified at load time.
 **/
typedef struct vdoLoadConfig {
  /** the offset on the physical layer where the VDO begins */
  PhysicalBlockNumber   firstBlockOffset;
  /** the expected release version number of the VDO */
  ReleaseVersionNumber  releaseVersion;
  /** the expected nonce of the VDO */
  Nonce                 nonce;
  /** the thread configuration of the VDO */
  ThreadConfig         *threadConfig;
  /** the page cache size, in pages */
  PageCount             cacheSize;
  /** whether writes are synchronous */
  WritePolicy           writePolicy;
  /** the maximum age of a dirty block map page in recovery journal blocks */
  BlockCount            maximumAge;
} VDOLoadConfig;

/**
 * Forward declarations of abstract types
 **/
struct action_manager;
struct allocating_vio;
struct allocation_selector;
struct block_allocator;
struct block_map;
struct block_map_tree_zone;
struct block_map_zone;
struct data_vio;
struct flusher;
struct forest;
struct hash_lock;
struct hash_zone;
typedef struct indexConfig         IndexConfig;
struct input_bin;
struct lbn_lock;
struct lock_counter;
struct logical_zone;
struct logical_zones;
struct pbn_lock;
typedef struct physicalLayer       PhysicalLayer;
struct physical_zone;
struct recovery_journal;
struct read_only_notifier;
struct ref_counts;
struct vdo_slab;
struct slab_depot;
struct slab_journal;
struct slab_journal_entry;
struct slab_scrubber;
struct slab_summary;
struct slab_summary_zone;
typedef struct vdo                 VDO;
struct vdo_completion;
typedef struct vdoExtent           VDOExtent;
struct vdo_flush;
struct vdo_layout;
typedef struct vdoStatistics       VDOStatistics;
struct vio;
struct vio_pool;

typedef struct {
  PhysicalBlockNumber pbn;
  BlockMappingState   state;
} DataLocation;

typedef struct {
  PhysicalBlockNumber   pbn;
  BlockMappingState     state;
  struct physical_zone *zone;
} ZonedPBN;

/**
 * Callback which will be called by the VDO when all of the vios in the
 * extent have been processed.
 *
 * @param extent The extent which is complete
 **/
typedef void VDOExtentCallback(VDOExtent *extent);

/**
 * An asynchronous operation.
 *
 * @param vio The vio on which to operate
 **/
typedef void AsyncOperation(struct vio *vio);

/**
 * An asynchronous compressed write operation.
 *
 * @param allocatingVIO  The allocating_vio to write
 **/
typedef void CompressedWriter(struct allocating_vio *allocatingVIO);

/**
 * An asynchronous data operation.
 *
 * @param dataVIO  The data_vio on which to operate
 **/
typedef void AsyncDataOperation(struct data_vio *dataVIO);

/**
 * A reference to a completion which (the reference) can be enqueued
 * for completion on a specified thread.
 **/
typedef struct enqueueable {
  struct vdo_completion *completion;
} Enqueueable;

#endif // TYPES_H
