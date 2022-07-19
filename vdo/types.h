/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef TYPES_H
#define TYPES_H

#include "compiler.h"
#include "type-defs.h"

#include "block-mapping-state.h"

/* A size type in blocks. */
typedef uint64_t block_count_t;

/* The size of a block. */
typedef uint16_t block_size_t;

/* A height within a tree. */
typedef uint8_t height_t;

/* The logical block number as used by the consumer. */
typedef uint64_t logical_block_number_t;

/* The type of the nonce used to identify instances of VDO. */
typedef uint64_t nonce_t;

/* A size in pages. */
typedef uint32_t page_count_t;

/* A page number. */
typedef uint32_t page_number_t;

/*
 * The physical (well, less logical) block number at which the block is found
 * on the underlying device.
 */
typedef uint64_t physical_block_number_t;

/*
 * A release version number. These numbers are used to make the numbering
 * space for component versions independent across release branches.
 *
 * Really an enum, but we have to specify the size for encoding; see
 * release_versions.h for the enumeration values.
 */
typedef uint32_t release_version_number_t;

/* A count of tree roots. */
typedef uint8_t root_count_t;

/* A number of sectors. */
typedef uint8_t sector_count_t;

/* A sequence number. */
typedef uint64_t sequence_number_t;

/* The offset of a block within a slab. */
typedef uint32_t slab_block_number;

/* A size type in slabs. */
typedef uint16_t slab_count_t;

/* A slot in a bin or block map page. */
typedef uint16_t slot_number_t;

/* A zone counter */
typedef uint8_t zone_count_t;

/*
 * The following enums are persisted on storage, so the values must be
 * preserved.
 */

/* The current operating mode of the VDO. */
enum vdo_state {
	VDO_DIRTY = 0,
	VDO_NEW = 1,
	VDO_CLEAN = 2,
	VDO_READ_ONLY_MODE = 3,
	VDO_FORCE_REBUILD = 4,
	VDO_RECOVERING = 5,
	VDO_REPLAYING = 6,
	VDO_REBUILD_FOR_UPGRADE = 7,

	/* Keep VDO_STATE_COUNT at the bottom. */
	VDO_STATE_COUNT
};

/*
 * The current operation on a physical block (from the point of view of the
 * recovery journal, slab journals, and reference counts.
 */
enum journal_operation {
	VDO_JOURNAL_DATA_DECREMENT = 0,
	VDO_JOURNAL_DATA_INCREMENT = 1,
	VDO_JOURNAL_BLOCK_MAP_DECREMENT = 2,
	VDO_JOURNAL_BLOCK_MAP_INCREMENT = 3,
} __packed;

/* Partition IDs encoded in the volume layout in the super block. */
enum partition_id {
	VDO_BLOCK_MAP_PARTITION = 0,
	VDO_BLOCK_ALLOCATOR_PARTITION = 1,
	VDO_RECOVERY_JOURNAL_PARTITION = 2,
	VDO_SLAB_SUMMARY_PARTITION = 3,
} __packed;

/* Metadata types for the vdo. */
enum vdo_metadata_type {
	VDO_METADATA_RECOVERY_JOURNAL = 1,
	VDO_METADATA_SLAB_JOURNAL,
} __packed;

/* A position in the block map where a block map entry is stored. */
struct block_map_slot {
	physical_block_number_t pbn;
	slot_number_t slot;
};

struct data_location {
	physical_block_number_t pbn;
	enum block_mapping_state state;
};

/*
 * The configuration of a single slab derived from the configured block size
 * and slab size.
 */
struct slab_config {
	/* total number of blocks in the slab */
	block_count_t slab_blocks;
	/* number of blocks available for data */
	block_count_t data_blocks;
	/* number of blocks for reference counts */
	block_count_t reference_count_blocks;
	/* number of blocks for the slab journal */
	block_count_t slab_journal_blocks;
	/*
	 * Number of blocks after which the slab journal starts pushing out a
	 * reference_block for each new entry it receives.
	 */
	block_count_t slab_journal_flushing_threshold;
	/*
	 * Number of blocks after which the slab journal pushes out all
	 * reference_blocks and makes all vios wait.
	 */
	block_count_t slab_journal_blocking_threshold;
	/*
	 * Number of blocks after which the slab must be scrubbed before coming
	 * online.
	 */
	block_count_t slab_journal_scrubbing_threshold;
} __packed;

struct vdo_config;

#endif /* TYPES_H */
