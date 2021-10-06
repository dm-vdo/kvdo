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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/kernel-types.h#1 $
 */

#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

#include "types.h"

/**
 * A count of compressed fragments
 **/
typedef uint8_t compressed_fragment_count_t;

/**
 * The size of a page.  Must be evenly divisible by block size.
 **/
typedef uint32_t page_size_t;

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
static const thread_id_t VDO_INVALID_THREAD_ID = (thread_id_t) -1;

/**
 * A number of vios.
 **/
typedef uint16_t vio_count_t;

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
 * Check whether a vio_type is for servicing an external data request.
 *
 * @param type  The vio_type to check
 **/
static inline bool is_vdo_data_vio_type(enum vio_type type)
{
	return (type == VIO_TYPE_DATA);
}

/**
 * Check whether a vio_type is for compressed block writes
 *
 * @param type  The vio_type to check
 **/
static inline bool is_vdo_compressed_write_vio_type(enum vio_type type)
{
	return (type == VIO_TYPE_COMPRESSED_BLOCK);
}

/**
 * Check whether a vio_type is for metadata
 *
 * @param type  The vio_type to check
 **/
static inline bool is_vdo_metadata_vio_type(enum vio_type type)
{
	return ((type != VIO_TYPE_UNINITIALIZED) &&
		!is_vdo_data_vio_type(type) &&
		!is_vdo_compressed_write_vio_type(type));
}

enum vdo_work_item_priority {
	BIO_ACK_Q_ACK_PRIORITY = 0,
	BIO_ACK_Q_MAX_PRIORITY = 0,
	BIO_Q_COMPRESSED_DATA_PRIORITY = 0,
	BIO_Q_DATA_PRIORITY = 0,
	BIO_Q_FLUSH_PRIORITY = 2,
	BIO_Q_HIGH_PRIORITY = 2,
	BIO_Q_METADATA_PRIORITY = 1,
	BIO_Q_VERIFY_PRIORITY = 1,
	BIO_Q_MAX_PRIORITY = 2,
	CPU_Q_COMPLETE_VIO_PRIORITY = 0,
	CPU_Q_COMPRESS_BLOCK_PRIORITY = 0,
	CPU_Q_EVENT_REPORTER_PRIORITY = 0,
	CPU_Q_HASH_BLOCK_PRIORITY = 0,
	CPU_Q_MAX_PRIORITY = 0,
	UDS_Q_PRIORITY = 0,
	UDS_Q_MAX_PRIORITY = 0,
	VDO_REQ_Q_COMPLETION_PRIORITY = 1,
	VDO_REQ_Q_FLUSH_PRIORITY = 2,
	VDO_REQ_Q_MAP_BIO_PRIORITY = 0,
	VDO_REQ_Q_SYNC_PRIORITY = 2,
	VDO_REQ_Q_VIO_CALLBACK_PRIORITY = 1,
	VDO_REQ_Q_MAX_PRIORITY = 2,
};

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

enum vdo_zone_type {
	VDO_ZONE_TYPE_ADMIN,
	VDO_ZONE_TYPE_JOURNAL,
	VDO_ZONE_TYPE_LOGICAL,
	VDO_ZONE_TYPE_PHYSICAL,
};

/**
 * Forward declarations of abstract types
 **/
struct action_manager;
struct allocating_vio;
struct allocation_selector;
struct atomic_bio_stats;
struct block_allocator;
struct block_map;
struct block_map_tree_zone;
struct block_map_zone;
struct data_vio;
struct dedupe_context;
struct dedupe_index;
struct device_config;
struct flusher;
struct forest;
struct hash_lock;
struct hash_zone;
struct index_config;
struct input_bin;
struct io_submitter;
struct lbn_lock;
struct lock_counter;
struct logical_zone;
struct logical_zones;
struct pbn_lock;
struct physical_zone;
struct read_only_notifier;
struct recovery_journal;
struct ref_counts;
struct slab_depot;
struct slab_journal;
struct slab_journal_entry;
struct slab_scrubber;
struct slab_summary;
struct slab_summary_zone;
struct thread_config;
struct thread_count_config;
struct vdo;
struct vdo_completion;
struct vdo_extent;
struct vdo_flush;
struct vdo_layout;
struct vdo_slab;
struct vdo_statistics;
struct vdo_thread;
struct vdo_work_item;
struct vdo_work_queue;
struct vio;
struct vio_pool;

struct zoned_pbn {
	physical_block_number_t pbn;
	enum block_mapping_state state;
	struct physical_zone *zone;
};

typedef void (*vdo_work_function)(struct vdo_work_item *work_item);

#endif /* KERNEL_TYPES_H */
