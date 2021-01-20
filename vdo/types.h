/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/types.h#74 $
 */

#ifndef TYPES_H
#define TYPES_H

#include "blockMappingState.h"
#include "common.h"
#include "statusCodes.h"

/**
 * A size type in blocks.
 **/
typedef uint64_t block_count_t;

/**
 * The size of a block.
 **/
typedef uint16_t block_size_t;

/**
 * A count of compressed fragments
 **/
typedef uint8_t compressed_fragment_count_t;

/**
 * A CRC-32 checksum
 **/
typedef uint32_t crc32_checksum_t;

/**
 * A height within a tree.
 **/
typedef uint8_t height_t;

/**
 * The logical block number as used by the consumer.
 **/
typedef uint64_t logical_block_number_t;

/**
 * The type of the nonce used to identify instances of VDO.
 **/
typedef uint64_t nonce_t;

/**
 * A size in pages.
 **/
typedef uint32_t page_count_t;

/**
 * A page number.
 **/
typedef uint32_t page_number_t;

/**
 * The size of a page.  Must be evenly divisible by block size.
 **/
typedef uint32_t page_size_t;

/**
 * The physical (well, less logical) block number at which the block is found
 * on the underlying device.
 **/
typedef uint64_t physical_block_number_t;

/**
 * A release version number. These numbers are used to make the numbering
 * space for component versions independent across release branches.
 *
 * Really an enum, but we have to specify the size for encoding; see
 * releaseVersions.h for the enumeration values.
 **/
typedef uint32_t release_version_number_t;

/**
 * A count of tree roots.
 **/
typedef uint8_t root_count_t;

/**
 * A number of sectors.
 **/
typedef uint8_t sector_count_t;

/**
 * A sequence number.
 **/
typedef uint64_t sequence_number_t;

/**
 * The offset of a block within a slab.
 **/
typedef uint32_t slab_block_number;

/**
 * A size type in slabs.
 **/
typedef uint16_t slab_count_t;

/**
 * A slot in a bin or block map page.
 **/
typedef uint16_t slot_number_t;

/**
 * A number of vios.
 **/
typedef uint16_t vio_count_t;

/**
 * A VDO thread configuration.
 **/
struct thread_config;

/**
 * A thread counter
 **/
typedef uint8_t thread_count_t;

/**
 * A thread ID
 *
 * Base-code threads are numbered sequentially starting from 0.
 **/
typedef uint8_t thread_id_t;

/**
 * The thread ID returned when the current base code thread ID cannot be found
 * or is otherwise undefined.
 **/
static const thread_id_t INVALID_THREAD_ID = (thread_id_t) -1;

/**
 * A zone counter
 **/
typedef uint8_t zone_count_t;

/**
 * The type of request a vio is performing
 **/
enum vio_operation {
	VIO_UNSPECIFIED_OPERATION = 0,
	VIO_READ = 1,
	VIO_WRITE = 2,
	VIO_READ_MODIFY_WRITE = VIO_READ | VIO_WRITE,
	VIO_READ_WRITE_MASK = VIO_READ_MODIFY_WRITE,
	VIO_FLUSH_BEFORE = 4,
	VIO_FLUSH_AFTER = 8,
} __packed;

/**
 * vio types for statistics and instrumentation.
 **/
enum vio_type {
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
} __packed;

/**
 * The current operation on a physical block (from the point of view of the
 * recovery journal, slab journals, and reference counts.
 **/
enum journal_operation {
	DATA_DECREMENT = 0,
	DATA_INCREMENT = 1,
	BLOCK_MAP_DECREMENT = 2,
	BLOCK_MAP_INCREMENT = 3,
} __packed;

/**
 * Partition IDs are encoded in the volume layout in the super block.
 **/
enum partition_id {
	BLOCK_MAP_PARTITION = 0,
	BLOCK_ALLOCATOR_PARTITION = 1,
	RECOVERY_JOURNAL_PARTITION = 2,
	SLAB_SUMMARY_PARTITION = 3,
} __packed;

/**
 * Check whether a vio_type is for servicing an external data request.
 *
 * @param type  The vio_type to check
 **/
static inline bool is_data_vio_type(enum vio_type type)
{
	return (type == VIO_TYPE_DATA);
}

/**
 * Check whether a vio_type is for compressed block writes
 *
 * @param type  The vio_type to check
 **/
static inline bool is_compressed_write_vio_type(enum vio_type type)
{
	return (type == VIO_TYPE_COMPRESSED_BLOCK);
}

/**
 * Check whether a vio_type is for metadata
 *
 * @param type  The vio_type to check
 **/
static inline bool is_metadata_vio_type(enum vio_type type)
{
	return ((type != VIO_TYPE_UNINITIALIZED) &&
		!is_data_vio_type(type) &&
		!is_compressed_write_vio_type(type));
}

/**
 * Priority levels for asynchronous I/O operations performed on a vio.
 **/
enum vio_priority {
	VIO_PRIORITY_LOW = 0,
	VIO_PRIORITY_DATA = VIO_PRIORITY_LOW,
	VIO_PRIORITY_COMPRESSED_DATA = VIO_PRIORITY_DATA,
	VIO_PRIORITY_METADATA,
	VIO_PRIORITY_HIGH,
} __packed;

/**
 * Metadata types for the vdo.
 **/
enum vdo_metadata_type {
	VDO_METADATA_RECOVERY_JOURNAL = 1,
	VDO_METADATA_SLAB_JOURNAL,
} __packed;

/**
 * The possible write policy values.
 **/
enum write_policy {
	/**
	 * All writes are synchronous, i. e., they are acknowledged
	 * only when the data is written to stable storage.
	 */
	WRITE_POLICY_SYNC,
	/**
	 * Writes are acknowledged when the data is cached for writing
	 * to stable storage, subject to resiliency guarantees
	 * specified elsewhere. After a crash, the data will be either
	 * old or new value for unflushed writes, never garbage.
	 */
	WRITE_POLICY_ASYNC,
	/**
	 * Writes are acknowledged when the data is cached for writing
	 * to stable storage, subject to resiliency guarantees
	 * specified elsewhere.
	 */
	WRITE_POLICY_ASYNC_UNSAFE,
	/**
	 * The appropriate policy is chosen based on the underlying device.
	 */
	WRITE_POLICY_AUTO,
};

enum vdo_zone_type {
	ZONE_TYPE_ADMIN,
	ZONE_TYPE_JOURNAL,
	ZONE_TYPE_LOGICAL,
	ZONE_TYPE_PHYSICAL,
};

/**
 * A position in the block map where a block map entry is stored.
 **/
struct block_map_slot {
	physical_block_number_t pbn;
	slot_number_t slot;
};

/**
 * A position in the arboreal block map at a specific level.
 **/
struct block_map_tree_slot {
	page_number_t page_index;
	struct block_map_slot block_map_slot;
};

/**
 * The configuration of a single slab derived from the configured block size
 * and slab size.
 **/
struct slab_config {
	/** total number of blocks in the slab */
	block_count_t slab_blocks;
	/** number of blocks available for data */
	block_count_t data_blocks;
	/** number of blocks for reference counts */
	block_count_t reference_count_blocks;
	/** number of blocks for the slab journal */
	block_count_t slab_journal_blocks;
	/**
	 * Number of blocks after which the slab journal starts pushing out a
	 * reference_block for each new entry it receives.
	 **/
	block_count_t slab_journal_flushing_threshold;
	/**
	 * Number of blocks after which the slab journal pushes out all
	 * reference_blocks and makes all vios wait.
	 **/
	block_count_t slab_journal_blocking_threshold;
	/**
	 * Number of blocks after which the slab must be scrubbed before coming
	 * online.
	 **/
	block_count_t slab_journal_scrubbing_threshold;
} __packed;

/**
 * The configuration of the VDO service.
 **/
struct vdo_config {
	block_count_t logical_blocks; ///< number of logical blocks
	block_count_t physical_blocks; ///< number of physical blocks
	block_count_t slab_size; ///< number of blocks in a slab
	block_count_t recovery_journal_size; ///< number of recovery journal blocks
	block_count_t slab_journal_blocks; ///< number of slab journal blocks
} __packed;

/**
 * The configuration parameters of the vdo service specified at load time.
 **/
struct vdo_load_config {
	/** the offset on the physical layer where the VDO begins */
	physical_block_number_t first_block_offset;
	/** the expected release version number of the VDO */
	release_version_number_t release_version;
	/** the expected nonce of the VDO */
	nonce_t nonce;
	/** the thread configuration of the VDO */
	struct thread_config *thread_config;
	/** the page cache size, in pages */
	page_count_t cache_size;
	/** whether writes are synchronous */
	enum write_policy write_policy;
	/**
	 * the maximum age of a dirty block map page in recovery journal blocks
	 */
	block_count_t maximum_age;
};

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
struct index_config;
struct input_bin;
struct lbn_lock;
struct lock_counter;
struct logical_zone;
struct logical_zones;
struct pbn_lock;
typedef struct physicalLayer PhysicalLayer;
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
struct vdo;
struct vdo_completion;
struct vdo_extent;
struct vdo_flush;
struct vdo_layout;
struct vdo_statistics;
struct vio;
struct vio_pool;

struct data_location {
	physical_block_number_t pbn;
	enum block_mapping_state state;
};

struct zoned_pbn {
	physical_block_number_t pbn;
	enum block_mapping_state state;
	struct physical_zone *zone;
};

/**
 * Callback which will be called by the VDO when all of the vios in the
 * extent have been processed.
 *
 * @param extent The extent which is complete
 **/
typedef void vdo_extent_callback(struct vdo_extent *extent);

/**
 * An asynchronous operation.
 *
 * @param vio The vio on which to operate
 **/
typedef void async_operation(struct vio *vio);

/**
 * An asynchronous compressed write operation.
 *
 * @param allocating_vio  The allocating_vio to write
 **/
typedef void compressed_writer(struct allocating_vio *allocating_vio);

/**
 * An asynchronous data operation.
 *
 * @param data_vio  The data_vio on which to operate
 **/
typedef void async_data_operation(struct data_vio *data_vio);

#endif // TYPES_H
