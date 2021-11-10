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
 */

#ifndef VDO_H
#define VDO_H

#include <linux/blk_types.h>
#include <linux/atomic.h>
#include <linux/kobject.h>
#include <linux/list.h>

#include "threadRegistry.h"

#include "admin-completion.h"
#include "admin-state.h"
#include "atomic-stats.h"
#include "device-config.h"
#include "header.h"
#include "limiter.h"
#include "packer.h"
#include "statistics.h"
#include "super-block.h"
#include "read-only-notifier.h"
#include "types.h"
#include "uds.h"
#include "vdo-component.h"
#include "vdo-component-states.h"
#include "vdo-layout.h"
#include "vdo-state.h"
#include "volume-geometry.h"


struct vdo_thread {
	struct vdo *vdo;
	thread_id_t thread_id;
	struct vdo_work_queue *queue;
	struct registered_thread allocating_thread;
};

struct vdo {
	struct vdo_thread *threads;
	struct vdo_work_item work_item;
	vdo_action *action;
	struct vdo_completion *completion;
	struct vio_tracer *vio_tracer;

	/** The connection to the UDS index */
	struct dedupe_index *dedupe_index;
	/** The pool of data_vios for handling incoming bios */
	struct buffer_pool *data_vio_pool;
	/* For returning batches of data_vios to their pool */
	struct batch_processor *data_vio_releaser;

	/* The atomic version of the state of this vdo */
	atomic_t state;
	/* The full state of all components */
	struct vdo_component_states states;
	/**
	 * A counter value to attach to thread names and log messages to
	 * identify the individual device.
	 **/
	unsigned int instance;
	/* The read-only notifier */
	struct read_only_notifier *read_only_notifier;
	/* The load-time configuration of this vdo */
	struct device_config *device_config;
	/* The thread mapping */
	struct thread_config *thread_config;

	/* The super block */
	struct vdo_super_block *super_block;

	/* Our partitioning of the physical layer's storage */
	struct vdo_layout *layout;

	/* The block map */
	struct block_map *block_map;

	/* The journal for block map recovery */
	struct recovery_journal *recovery_journal;

	/* The slab depot */
	struct slab_depot *depot;

	/* The compressed-block packer */
	struct packer *packer;
	/* Whether incoming data should be compressed */
	bool compressing;

	/* The handler for flush requests */
	struct flusher *flusher;

	/* The state the vdo was in when loaded (primarily for unit tests) */
	enum vdo_state load_state;

	/* The logical zones of this vdo */
	struct logical_zones *logical_zones;

	/* The physical zones of this vdo */
	struct physical_zone *physical_zones;

	/* The hash lock zones of this vdo */
	struct hash_zone **hash_zones;

	/**
	 * Bio submission manager used for sending bios to the storage
	 * device.
	 **/
	struct io_submitter *io_submitter;

	/* The completion for administrative operations */
	struct admin_completion admin_completion;

	/* The administrative state of the vdo */
	struct admin_state admin_state;

	/* Flags controlling administrative operations */
	const struct admin_state_code *suspend_type;
	bool allocations_allowed;
	bool dump_on_shutdown;
	atomic_t processing_message;

	/*
	 * Statistics
	 * Atomic stats counters
	 */
	struct atomic_statistics stats;
	/* Used to gather statistics without allocating memory */
	struct vdo_statistics stats_buffer;
	/* Protects the stats_buffer */
	struct mutex stats_mutex;
	/* true if sysfs directory is set up */
	bool sysfs_added;
	/* Used when shutting down the sysfs statistics */
	struct completion stats_shutdown;


	/** A list of all device_configs referencing this vdo */
	struct list_head device_config_list;

	/** This VDO's list entry for the device registry */
	struct list_head registration;

	/** Underlying block device info. */
	uint64_t starting_sector_offset;
	struct volume_geometry geometry;

	/* For sysfs */
	struct kobject vdo_directory;
	struct kobject stats_directory;

	/** Limit the number of requests that are being processed. */
	struct limiter request_limiter;
	struct limiter discard_limiter;

	/** N blobs of context data for LZ4 code, one per CPU thread. */
	char **compression_context;
};

/**
 * Indicate whether the vdo is configured to use a separate work queue for
 * acknowledging received and processed bios.
 *
 * Note that this directly controls the handling of write operations, but the
 * compile-time flag VDO_USE_BIO_ACK_QUEUE_FOR_READ is also checked for read
 * operations.
 *
 * @param vdo  The vdo
 *
 * @return Whether a bio-acknowledgement work queue is in use
 **/
static inline bool vdo_uses_bio_ack_queue(struct vdo *vdo)
{
	return vdo->device_config->thread_counts.bio_ack_threads > 0;
}

int __must_check
make_vdo_thread(struct vdo *vdo,
		const char *thread_name_prefix,
		thread_id_t thread_id,
		const struct vdo_work_queue_type *type,
		unsigned int queue_count,
		void *contexts[]);

int __must_check
make_vdo(unsigned int instance,
	 struct device_config *config,
	 char **reason,
	 struct vdo **vdo_ptr);

void destroy_vdo(struct vdo *vdo);

int __must_check add_vdo_sysfs_stats_dir(struct vdo *vdo);

int __must_check
prepare_to_modify_vdo(struct vdo *vdo,
		      struct device_config *config,
		      bool may_grow,
		      char **error_ptr);

struct block_device * __must_check
get_vdo_backing_device(const struct vdo *vdo);

int __must_check vdo_synchronous_flush(struct vdo *vdo);

const struct admin_state_code * __must_check
get_vdo_admin_state(const struct vdo *vdo);

bool set_vdo_compressing(struct vdo *vdo, bool enable);

bool get_vdo_compressing(struct vdo *vdo);

void fetch_vdo_statistics(struct vdo *vdo, struct vdo_statistics *stats);

thread_id_t vdo_get_callback_thread_id(void);

struct zoned_pbn __must_check
vdo_validate_dedupe_advice(struct vdo *vdo,
			   const struct data_location *advice,
			   logical_block_number_t lbn);

enum vdo_state __must_check get_vdo_state(const struct vdo *vdo);

void set_vdo_state(struct vdo *vdo, enum vdo_state state);

void save_vdo_components(struct vdo *vdo, struct vdo_completion *parent);

int enable_vdo_read_only_entry(struct vdo *vdo);

bool __must_check in_vdo_read_only_mode(const struct vdo *vdo);

bool __must_check in_vdo_recovery_mode(const struct vdo *vdo);

void enter_vdo_recovery_mode(struct vdo *vdo);

void assert_on_vdo_admin_thread(const struct vdo *vdo, const char *name);

void assert_on_vdo_logical_zone_thread(const struct vdo *vdo,
				       zone_count_t logical_zone,
				       const char *name);

void assert_on_vdo_physical_zone_thread(const struct vdo *vdo,
					zone_count_t physical_zone,
					const char *name);

struct hash_zone * __must_check
select_vdo_hash_zone(const struct vdo *vdo, const struct uds_chunk_name *name);

int __must_check get_vdo_physical_zone(const struct vdo *vdo,
				       physical_block_number_t pbn,
				       struct physical_zone **zone_ptr);

zone_count_t __must_check
get_vdo_bio_zone(const struct vdo *vdo, physical_block_number_t pbn);

void dump_vdo_status(const struct vdo *vdo);

#endif /* VDO_H */
