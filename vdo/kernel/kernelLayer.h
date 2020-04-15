/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelLayer.h#37 $
 */

#ifndef KERNELLAYER_H
#define KERNELLAYER_H

#include <linux/device-mapper.h>

#include "atomic.h"
#include "constants.h"
#include "flush.h"
#include "intMap.h"
#include "physicalLayer.h"
#include "ringNode.h"
#include "volumeGeometry.h"
#include "waitQueue.h"

#include "batchProcessor.h"
#include "bufferPool.h"
#include "deadlockQueue.h"
#include "deviceConfig.h"
#include "histogram.h"
#include "kernelStatistics.h"
#include "kernelTypes.h"
#include "kernelVDO.h"
#include "ktrace.h"
#include "limiter.h"
#include "statistics.h"
#include "workQueue.h"

enum {
	VDO_SECTORS_PER_BLOCK = (VDO_BLOCK_SIZE >> SECTOR_SHIFT)
};

typedef enum {
	LAYER_SIMPLE_THINGS_INITIALIZED,
	LAYER_BUFFER_POOLS_INITIALIZED,
	LAYER_REQUEST_QUEUE_INITIALIZED,
	LAYER_CPU_QUEUE_INITIALIZED,
	LAYER_BIO_ACK_QUEUE_INITIALIZED,
	LAYER_BIO_DATA_INITIALIZED,
	LAYER_STARTING,
	LAYER_RUNNING,
	LAYER_SUSPENDED,
	LAYER_STOPPING,
	LAYER_STOPPED,
	LAYER_RESUMING,
} kernel_layer_state;

/* Keep struct bio statistics atomically */
struct atomic_bio_stats {
	atomic64_t read; // Number of not REQ_WRITE bios
	atomic64_t write; // Number of REQ_WRITE bios
	atomic64_t discard; // Number of REQ_DISCARD bios
	atomic64_t flush; // Number of REQ_FLUSH bios
	atomic64_t fua; // Number of REQ_FUA bios
};

/**
 * The VDO representation of the target device
 **/
struct kernel_layer {
	PhysicalLayer common;
	// Layer specific info
	struct device_config *device_config;
	/** A ring of all DeviceConfigs referencing this layer */
	RingNode device_config_ring;
	char thread_name_prefix[MAX_QUEUE_NAME_LEN];
	struct kobject kobj;
	struct kobject wq_directory;
	struct kobject statsDirectory;
	/**
	 * A counter value to attach to thread names and log messages to
	 * identify the individual device.
	 **/
	unsigned int instance;
	/** Contains the current kernel_layer_state, which rarely changes */
	Atomic32 state;
	bool no_flush_suspend;
	bool allocations_allowed;
	AtomicBool processing_message;
	struct dm_target_callbacks callbacks;

	/** Limit the number of requests that are being processed. */
	struct limiter request_limiter;
	struct limiter discard_limiter;
	struct kvdo kvdo;
	/** Incoming bios we've had to buffer to avoid deadlock. */
	struct deadlock_queue deadlock_queue;
	// for REQ_FLUSH processing
	struct bio_list waiting_flushes;
	struct kvdo_flush *spare_kvdo_flush;
	spinlock_t flush_lock;
	Jiffies flush_arrival_time;
	/**
	 * Bio submission manager used for sending bios to the storage
	 * device.
	 **/
	struct io_submitter *io_submitter;
	/**
	 * Work queue (possibly with multiple threads) for miscellaneous
	 * CPU-intensive, non-blocking work.
	 **/
	struct kvdo_work_queue *cpu_queue;
	/** N blobs of context data for LZ4 code, one per CPU thread. */
	char **compressionContext;
	/** Optional work queue for calling bio_endio. */
	struct kvdo_work_queue *bio_ack_queue;
	/** Underlying block device info. */
	uint64_t starting_sector_offset;
	struct volume_geometry geometry;
	// Memory allocation
	struct buffer_pool *data_kvio_pool;
	// Albireo specific info
	struct dedupe_index *dedupe_index;
	// Statistics
	atomic64_t bios_submitted;
	atomic64_t bios_completed;
	atomic64_t dedupeContextBusy;
	atomic64_t flushOut;
	struct atomic_bio_stats biosIn;
	struct atomic_bio_stats biosInPartial;
	struct atomic_bio_stats biosOut;
	struct atomic_bio_stats biosOutCompleted;
	struct atomic_bio_stats biosAcknowledged;
	struct atomic_bio_stats biosAcknowledgedPartial;
	struct atomic_bio_stats biosMeta;
	struct atomic_bio_stats biosMetaCompleted;
	struct atomic_bio_stats biosJournal;
	struct atomic_bio_stats biosPageCache;
	struct atomic_bio_stats biosJournalCompleted;
	struct atomic_bio_stats biosPageCacheCompleted;
	// Debugging
	/* Whether to dump VDO state on shutdown */
	bool dump_on_shutdown;
	/**
	 * Whether we should collect tracing info. (Actually, this controls
	 * allocations; non-null record pointers cause recording.)
	 **/
	bool vioTraceRecording;
	struct sample_counter trace_sample_counter;
	/* Should we log tracing info? */
	bool trace_logging;
	/* Storage for trace data. */
	struct buffer_pool *trace_buffer_pool;
	/* For returning batches of DataKVIOs to their pool */
	struct batch_processor *data_kvio_releaser;

	// Administrative operations
	/* The object used to wait for administrative operations to complete */
	struct completion callbackSync;

	// Statistics reporting
	/* Protects the *statsStorage structs */
	struct mutex statsMutex;
	/* Used when shutting down the sysfs statistics */
	struct completion stats_shutdown;

	/* true if sysfs statistics directory is set up */
	bool stats_added;
	/* Used to gather statistics without allocating memory */
	struct vdo_statistics vdo_stats_storage;
	struct kernel_statistics kernel_stats_storage;
};

typedef enum {
	BIO_Q_ACTION_COMPRESSED_DATA,
	BIO_Q_ACTION_DATA,
	BIO_Q_ACTION_FLUSH,
	BIO_Q_ACTION_HIGH,
	BIO_Q_ACTION_METADATA,
	BIO_Q_ACTION_READCACHE,
	BIO_Q_ACTION_VERIFY
} bio_q_action;

typedef enum {
	CPU_Q_ACTION_COMPLETE_KVIO,
	CPU_Q_ACTION_COMPRESS_BLOCK,
	CPU_Q_ACTION_EVENT_REPORTER,
	CPU_Q_ACTION_HASH_BLOCK,
} cpu_q_action;

typedef enum {
	BIO_ACK_Q_ACTION_ACK,
} bio_ack_q_action;

typedef void (*dedupe_shutdown_callback_function)(struct kernel_layer *layer);

/*
 * Wrapper for the Enqueueable object, to associate it with a kernel
 * layer work item.
 */
struct kvdo_enqueueable {
	struct kvdo_work_item work_item;
	Enqueueable enqueueable;
};

/**
 * Implements LayerFilter.
 **/
bool __must_check layer_is_named(struct kernel_layer *layer, void *context);

/**
 * Creates a kernel specific physical layer to be used by VDO
 *
 * @param starting_sector        The sector offset of our table entry in the
 *                               DM device
 * @param instance               Device instantiation counter
 * @param parent_kobject         The parent sysfs node
 * @param config                 The device configuration
 * @param thread_config_pointer  Where to store the new threadConfig handle
 * @param reason                 The reason for any failure during this call
 * @param layer_ptr              A pointer to hold the created layer
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_kernel_layer(uint64_t starting_sector,
		  unsigned int instance,
		  struct device_config *config,
		  struct kobject *parent_kobject,
		  struct thread_config **thread_config_pointer,
		  char **reason,
		  struct kernel_layer **layer_ptr);

/**
 * Prepare to modify a kernel layer.
 *
 * @param layer      The layer to modify
 * @param config     The new device configuration
 * @param error_ptr  A pointer to store the reason for any failure
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
prepare_to_modify_kernel_layer(struct kernel_layer *layer,
			       struct device_config *config,
			       char **error_ptr);

/**
 * Modify a kernel physical layer.
 *
 * @param layer   The layer to modify
 * @param config  The new device configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
modify_kernel_layer(struct kernel_layer *layer, struct device_config *config);

/**
 * Free a kernel physical layer.
 *
 * @param layer    The layer, which must have been created by
 *                 make_kernel_layer
 **/
void free_kernel_layer(struct kernel_layer *layer);

/**
 * Make and configure a kernel layer. This method does not alter the VDO state
 * on disk. It should be run from the VDO constructor for devices which have
 * not been started.
 *
 * @param layer        The kernel layer
 * @param load_config  Load-time parameters for the VDO
 * @param reason       The reason for any failure during this call
 *
 * @return VDO_SUCCESS or an error
 *
 * @note redundant starts are silently ignored
 **/
int preload_kernel_layer(struct kernel_layer *layer,
			 const struct vdo_load_config *load_config,
			 char **reason);

/**
 * Start the kernel layer. This method finishes bringing a VDO online now that
 * a table is being resumed for the first time.
 *
 * @param layer   The kernel layer
 * @param reason  The reason for any failure during this call
 *
 * @return VDO_SUCCESS or an error
 **/
int start_kernel_layer(struct kernel_layer *layer, char **reason);

/**
 * Stop the kernel layer.
 *
 * @param layer  The kernel layer
 **/
void stop_kernel_layer(struct kernel_layer *layer);

/**
 * Suspend the kernel layer.
 *
 * @param layer  The kernel layer
 *
 * @return VDO_SUCCESS or an error
 **/
int suspend_kernel_layer(struct kernel_layer *layer);

/**
 * Resume the kernel layer.
 *
 * @param layer  The kernel layer
 *
 * @return VDO_SUCCESS or an error
 **/
int resume_kernel_layer(struct kernel_layer *layer);

/**
 * Get the kernel layer state.
 *
 * @param layer  The kernel layer
 *
 * @return the instantaneously correct kernel layer state
 **/
static inline kernel_layer_state
get_kernel_layer_state(const struct kernel_layer *layer)
{
	return atomicLoad32(&layer->state);
}

/**
 * Function call to begin processing a bio passed in from the block layer
 *
 * @param layer  The physical layer
 * @param bio    The bio from the block layer
 *
 * @return value to return from the VDO map function.  Either an error code
 *         or DM_MAPIO_REMAPPED or DM_MAPPED_SUBMITTED (see vdoMapBio for
 *         details).
 **/
int kvdo_map_bio(struct kernel_layer *layer, struct bio *bio);

/**
 * Convert a generic PhysicalLayer to a kernel_layer.
 *
 * @param layer The PhysicalLayer to convert
 *
 * @return The PhysicalLayer as a struct kernel_layer
 **/
static inline struct kernel_layer *as_kernel_layer(PhysicalLayer *layer)
{
	return container_of(layer, struct kernel_layer, common);
}

/**
 * Convert a block number (or count) to a (512-byte-)sector number.
 *
 * The argument type is sector_t to force conversion to the type we
 * want, although the actual values passed are of various integral
 * types.  It's just too easy to forget and do the multiplication
 * without casting, resulting in 32-bit arithmetic that accidentally
 * produces wrong results in devices over 2TB (2**32 sectors).
 *
 * @param [in] layer         the physical layer
 * @param [in] block_number  the block number/count
 *
 * @return      the sector number/count
 **/
static inline sector_t block_to_sector(struct kernel_layer *layer,
				       sector_t block_number)
{
	return (block_number * VDO_SECTORS_PER_BLOCK);
}

/**
 * Convert a sector number (or count) to a block number. Does not
 * check to make sure the sector number is an integral number of
 * blocks.
 *
 * @param [in] layer          the physical layer
 * @param [in] sector_number  the sector number/count
 *
 * @return      the block number/count
 **/
static inline sector_t sector_to_block(struct kernel_layer *layer,
				       sector_t sector_number)
{
	return (sector_number / VDO_SECTORS_PER_BLOCK);
}

/**
 * Convert a sector number to an offset within a block.
 *
 * @param [in] layer          the physical layer
 * @param [in] sector_number  the sector number
 *
 * @return      the offset within the block
 **/
static inline block_size_t sector_to_block_offset(struct kernel_layer *layer,
						  sector_t sector_number)
{
	unsigned int sectors_per_block_mask = VDO_SECTORS_PER_BLOCK - 1;
	return to_bytes(sector_number & sectors_per_block_mask);
}

/**
 * Get the block device object currently underlying a kernel layer.
 *
 * @param layer  The kernel layer in question
 *
 * @return The block device object under the layer
 **/
struct block_device * __must_check
get_kernel_layer_bdev(const struct kernel_layer *layer);

/**
 * Set the layer's active config.
 *
 * @param layer   The kernel layer in question
 * @param config  The config in question
 **/
static inline void set_kernel_layer_active_config(struct kernel_layer *layer,
						  struct device_config *config)
{
	layer->device_config = config;
}

/**
 * Given an error code, return a value we can return to the OS.  The
 * input error code may be a system-generated value (such as -EIO), an
 * errno macro used in our code (such as EIO), or a UDS or VDO status
 * code; the result must be something the rest of the OS can consume
 * (negative errno values such as -EIO, in the case of the kernel).
 *
 * @param error    the error code to convert
 *
 * @return   a system error code value
 **/
int map_to_system_error(int error);

/**
 * Wait until there are no requests in progress.
 *
 * @param layer  The kernel layer for the device
 **/
void wait_for_no_requests_active(struct kernel_layer *layer);

/**
 * Enqueues an item on our internal "cpu queues". Since there is more than
 * one, we rotate through them in hopes of creating some general balance.
 *
 * @param layer The kernel layer
 * @param item  The work item to enqueue
 */
static inline void enqueue_cpu_work_queue(struct kernel_layer *layer,
					  struct kvdo_work_item *item)
{
	enqueue_work_queue(layer->cpu_queue, item);
}

/**
 * Adjust parameters to prepare to use a larger physical space.
 * The size must be larger than the current size.
 *
 * @param layer           the kernel layer
 * @param physical_count  the new physical size in blocks
 *
 * @return VDO_SUCCESS or an error
 */
int prepare_to_resize_physical(struct kernel_layer *layer,
			       block_count_t physical_count);

/**
 * Adjusts parameters to reflect resizing the underlying device.
 * The size must be larger than the current size.
 *
 * @param layer           the kernel layer
 * @param physical_count  the new physical count in blocks
 *
 * @return VDO_SUCCESS or an error
 */
int resize_physical(struct kernel_layer *layer, block_count_t physical_count);

/**
 * Adjust parameters to prepare to present a larger logical space.
 * The size must be larger than the current size.
 *
 * @param layer          the kernel layer
 * @param logical_count  the new logical size in blocks
 *
 * @return VDO_SUCCESS or an error
 */
int prepare_to_resize_logical(struct kernel_layer *layer,
			      block_count_t logical_count);

/**
 * Adjust parameters to present a larger logical space.
 * The size must be larger than the current size.
 *
 * @param layer          the kernel layer
 * @param logical_count  the new logical size in blocks
 *
 * @return VDO_SUCCESS or an error
 */
int resize_logical(struct kernel_layer *layer, block_count_t logical_count);

/**
 * Indicate whether the kernel layer is configured to use a separate
 * work queue for acknowledging received and processed bios.
 *
 * Note that this directly controls handling of write operations, but
 * the compile-time flag USE_BIO_ACK_QUEUE_FOR_READ is also checked
 * for read operations.
 *
 * @param  layer  The kernel layer
 *
 * @return   Whether a bio-acknowledgement work queue is in use
 **/
static inline bool use_bio_ack_queue(struct kernel_layer *layer)
{
	return layer->device_config->thread_counts.bio_ack_threads > 0;
}

/**
 * Update bookkeeping for the completion of some number of requests, so that
 * more incoming requests can be accepted.
 *
 * @param layer  The kernel layer
 * @param count  The number of completed requests
 **/
void complete_many_requests(struct kernel_layer *layer, uint32_t count);

#endif /* KERNELLAYER_H */
