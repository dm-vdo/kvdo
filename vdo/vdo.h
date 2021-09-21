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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo.h#66 $
 */

#ifndef VDO_H
#define VDO_H

#include <linux/blk_types.h>
#include <linux/atomic.h>
#include <linux/kobject.h>
#include <linux/list.h>

#include "threadRegistry.h"

#include "adminCompletion.h"
#include "adminState.h"
#include "atomicStats.h"
#include "deviceConfig.h"
#include "header.h"
#include "limiter.h"
#include "packer.h"
#include "statistics.h"
#include "superBlock.h"
#include "readOnlyNotifier.h"
#include "types.h"
#include "uds.h"
#include "vdoComponent.h"
#include "vdoComponentStates.h"
#include "vdoLayout.h"
#include "vdoState.h"
#include "volumeGeometry.h"


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
	struct physical_zone **physical_zones;

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

	// Statistics
	/* Atomic stats counters */
	struct atomic_statistics stats;
	/* Used to gather statistics without allocating memory */
	struct vdo_statistics stats_buffer;
	/* Protects the stats_buffer */
	struct mutex stats_mutex;
	/* true if sysfs statistics directory is set up */
	bool stats_added;
	/* Used when shutting down the sysfs statistics */
	struct completion stats_shutdown;


	/** A list of all device_configs referencing this vdo */
	struct list_head device_config_list;

	/** This VDO's list entry for the device registry */
	struct list_head registration;

	/** Underlying block device info. */
	uint64_t starting_sector_offset;
	struct volume_geometry geometry;

	// For sysfs
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

/**
 * Construct a single vdo work_queue and its associated thread (or threads for
 * round-robin queues). Each "thread" constructed by this method is represented
 * by a unique thread id in the thread config, and completions can be enqueued
 * to the queue and run on the threads comprising this entity.
 *
 * @param vdo                 The vdo which owns the thread
 * @param thread_name_prefix  The per-device prefix for the thread name
 * @param thread_id           The id of the thread to create (as determined by
 *                            the thread_config)
 * @param type                The description of the work queue for this thread
 * @param queue_count         The number of actual threads/queues contained in
 *                            the "thread"
 * @param contexts            An array of queue_count contexts one for each
 *                            individual queue, may be NULL &
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_vdo_thread(struct vdo *vdo,
		const char *thread_name_prefix,
		thread_id_t thread_id,
		const struct vdo_work_queue_type *type,
		unsigned int queue_count,
		void *contexts[]);

/**
 * Allocate and initialize a vdo.
 *
 * @param instance   Device instantiation counter
 * @param config     The device configuration
 * @param reason     The reason for any failure during this call
 * @param vdo_ptr    A pointer to hold the created vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_vdo(unsigned int instance,
	 struct device_config *config,
	 char **reason,
	 struct vdo **vdo_ptr);

/**
 * Destroy a vdo instance.
 *
 * @param vdo  The vdo to destroy (may be NULL)
 **/
void destroy_vdo(struct vdo *vdo);

/**
 * Add the stats directory to the vdo sysfs directory.
 *
 * @param vdo  The vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check add_vdo_sysfs_stats_dir(struct vdo *vdo);

/**
 * Prepare to modify a vdo. This method is called during preresume to prepare
 * for modifications which could result if the table has changed.
 *
 * @param vdo        The vdo being resumed
 * @param config     The new device configuration
 * @param may_grow   Set to true if growing the logical and physical size of
 *                   the vdo is currently permitted
 * @param error_ptr  A pointer to store the reason for any failure
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
prepare_to_modify_vdo(struct vdo *vdo,
		      struct device_config *config,
		      bool may_grow,
		      char **error_ptr);

/**
 * Get the block device object underlying a vdo.
 *
 * @param vdo  The vdo
 *
 * @return The vdo's current block device
 **/
struct block_device * __must_check
get_vdo_backing_device(const struct vdo *vdo);

/**
 * Issue a flush request and wait for it to complete.
 *
 * @param vdo  The vdo
 *
 * @return VDO_SUCCESS or an error
 */
int __must_check vdo_synchronous_flush(struct vdo *vdo);

/**
 * Get the admin state of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The code for the vdo's current admin state
 **/
const struct admin_state_code * __must_check
get_vdo_admin_state(const struct vdo *vdo);

/**
 * Turn compression on or off.
 *
 * @param vdo     The vdo
 * @param enable  Whether to enable or disable compression
 *
 * @return Whether compression was previously on or off
 **/
bool set_vdo_compressing(struct vdo *vdo, bool enable);

/**
 * Get whether compression is enabled in a vdo.
 *
 * @param vdo  The vdo
 *
 * @return State of compression
 **/
bool get_vdo_compressing(struct vdo *vdo);

/**
 * Fetch statistics on the correct thread.
 *
 * @param [in]  vdo    The vdo
 * @param [out] stats  The vdo statistics are returned here
 **/
void fetch_vdo_statistics(struct vdo *vdo, struct vdo_statistics *stats);

/**
 * Get the id of the callback thread on which a completion is currently
 * running, or -1 if no such thread.
 *
 * @return the current thread ID
 **/
thread_id_t vdo_get_callback_thread_id(void);

/**
 * Check whether a data_location containing potential dedupe advice is
 * well-formed and addresses a data block in one of the configured physical
 * zones of the vdo. If it is, return the location and zone as a zoned_pbn;
 * otherwise increment statistics tracking invalid advice and return an
 * unmapped zoned_pbn.
 *
 * @param vdo     The vdo
 * @param advice  The advice to validate (NULL indicates no advice)
 * @param lbn     The logical block number of the write that requested advice,
 *                which is only used for debug-level logging of invalid advice
 *
 * @return The zoned_pbn representing the advice, if valid, otherwise an
 *         unmapped zoned_pbn if the advice was invalid or NULL
 **/
struct zoned_pbn __must_check
vdo_validate_dedupe_advice(struct vdo *vdo,
			   const struct data_location *advice,
			   logical_block_number_t lbn);

/**
 * Get the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo  The vdo
 *
 * @return the current state of the vdo
 **/
enum vdo_state __must_check get_vdo_state(const struct vdo *vdo);

/**
 * Set the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo    The vdo whose state is to be set
 * @param state  The new state of the vdo
 **/
void set_vdo_state(struct vdo *vdo, enum vdo_state state);

/**
 * Encode the vdo and save the super block asynchronously. All non-user mode
 * super block savers should use this bottle neck instead of calling
 * save_vdo_super_block() directly.
 *
 * @param vdo     The vdo whose state is being saved
 * @param parent  The completion to notify when the save is complete
 **/
void save_vdo_components(struct vdo *vdo, struct vdo_completion *parent);

/**
 * Enable a vdo to enter read-only mode on errors.
 *
 * @param vdo  The vdo to enable
 *
 * @return VDO_SUCCESS or an error
 **/
int enable_vdo_read_only_entry(struct vdo *vdo);

/**
 * Check whether a vdo is in read-only mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in read-only mode
 **/
bool __must_check in_vdo_read_only_mode(const struct vdo *vdo);

/**
 * Check whether the vdo is in recovery mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in recovery mode
 **/
bool __must_check in_vdo_recovery_mode(const struct vdo *vdo);

/**
 * Put the vdo into recovery mode
 *
 * @param vdo  The vdo
 **/
void enter_vdo_recovery_mode(struct vdo *vdo);

/**
 * Assert that we are running on the admin thread.
 *
 * @param vdo   The vdo
 * @param name  The name of the function which should be running on the admin
 *              thread (for logging).
 **/
void assert_on_vdo_admin_thread(const struct vdo *vdo, const char *name);

/**
 * Assert that this function was called on the specified logical zone thread.
 *
 * @param vdo           The vdo
 * @param logical_zone  The number of the logical zone
 * @param name          The name of the calling function
 **/
void assert_on_vdo_logical_zone_thread(const struct vdo *vdo,
				       zone_count_t logical_zone,
				       const char *name);

/**
 * Assert that this function was called on the specified physical zone thread.
 *
 * @param vdo            The vdo
 * @param physical_zone  The number of the physical zone
 * @param name           The name of the calling function
 **/
void assert_on_vdo_physical_zone_thread(const struct vdo *vdo,
					zone_count_t physical_zone,
					const char *name);

/**
 * Select the hash zone responsible for locking a given chunk name.
 *
 * @param vdo   The vdo containing the hash zones
 * @param name  The chunk name
 *
 * @return  The hash zone responsible for the chunk name
 **/
struct hash_zone * __must_check
select_vdo_hash_zone(const struct vdo *vdo, const struct uds_chunk_name *name);

/**
 * Get the physical zone responsible for a given physical block number of a
 * data block in this vdo instance, or of the zero block (for which a NULL
 * zone is returned). For any other block number that is not in the range of
 * valid data block numbers in any slab, an error will be returned. This
 * function is safe to call on invalid block numbers; it will not put the vdo
 * into read-only mode.
 *
 * @param [in]  vdo       The vdo containing the physical zones
 * @param [in]  pbn       The PBN of the data block
 * @param [out] zone_ptr  A pointer to return the physical zone
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE if the block number is invalid
 *         or an error code for any other failure
 **/
int __must_check get_vdo_physical_zone(const struct vdo *vdo,
				       physical_block_number_t pbn,
				       struct physical_zone **zone_ptr);

/**
 * Dump status information about a vdo to the log for debugging.
 *
 * @param vdo  The vdo to dump
 **/
void dump_vdo_status(const struct vdo *vdo);

#endif /* VDO_H */
