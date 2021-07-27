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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelLayer.h#97 $
 */

#ifndef KERNELLAYER_H
#define KERNELLAYER_H

#include <linux/atomic.h>
#include <linux/device-mapper.h>
#include <linux/list.h>

#include "constants.h"
#include "flush.h"
#include "intMap.h"
#include "types.h"
#include "vdo.h"
#include "vdoInternal.h"
#include "waitQueue.h"

#include "batchProcessor.h"
#include "bufferPool.h"
#include "deadlockQueue.h"
#include "deviceConfig.h"
#include "kernelTypes.h"
#include "kernelVDO.h"
#include "limiter.h"
#include "statistics.h"
#include "workQueue.h"

enum kernel_layer_state {
	LAYER_NEW,
	LAYER_STARTING,
	LAYER_RUNNING,
	LAYER_SUSPENDED,
	LAYER_STOPPING,
	LAYER_STOPPED,
	LAYER_RESUMING,
};

/**
 * The VDO representation of the target device
 **/
struct kernel_layer {
	struct vdo vdo;
	/** Accessed from multiple threads */
	enum kernel_layer_state state;
};

enum bio_q_action {
	BIO_Q_ACTION_COMPRESSED_DATA,
	BIO_Q_ACTION_DATA,
	BIO_Q_ACTION_FLUSH,
	BIO_Q_ACTION_HIGH,
	BIO_Q_ACTION_METADATA,
	BIO_Q_ACTION_VERIFY
};

enum cpu_q_action {
	CPU_Q_ACTION_COMPLETE_VIO,
	CPU_Q_ACTION_COMPRESS_BLOCK,
	CPU_Q_ACTION_EVENT_REPORTER,
	CPU_Q_ACTION_HASH_BLOCK,
};

enum bio_ack_q_action {
	BIO_ACK_Q_ACTION_ACK,
};

/**
 * Creates a kernel specific physical layer to be used by VDO
 *
 * @param instance   Device instantiation counter
 * @param config     The device configuration
 * @param reason     The reason for any failure during this call
 * @param layer_ptr  A pointer to hold the created layer
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_kernel_layer(unsigned int instance,
		  struct device_config *config,
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
 * @param layer  The layer, which must have been created by
 *               make_kernel_layer
 **/
void free_kernel_layer(struct kernel_layer *layer);

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
static inline enum kernel_layer_state
get_kernel_layer_state(const struct kernel_layer *layer)
{
	enum kernel_layer_state state = READ_ONCE(layer->state);
	smp_rmb();
	return state;
}

/**
 * Function call to begin processing a bio passed in from the block layer
 *
 * @param vdo  The VDO instance
 * @param bio  The bio from the block layer
 *
 * @return value to return from the VDO map function.  Either an error code
 *         or DM_MAPIO_REMAPPED or DM_MAPPED_SUBMITTED (see vdo_map_bio for
 *         details).
 **/
int vdo_launch_bio(struct vdo *vdo, struct bio *bio);

/**
 * Convert a struct vdo pointer to the kernel_layer contining it.
 *
 * @param vdo  The vdo to convert
 *
 * @return The enclosing struct kernel_layer
 **/
static inline struct kernel_layer *vdo_as_kernel_layer(struct vdo *vdo)
{
	return container_of(vdo, struct kernel_layer, vdo);
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
 * @param block_number  the block number/count
 *
 * @return the sector number/count
 **/
static inline sector_t block_to_sector(physical_block_number_t block_number)
{
	return (block_number * VDO_SECTORS_PER_BLOCK);
}

/**
 * Convert a sector number (or count) to a block number. Does not
 * check to make sure the sector number is an integral number of
 * blocks.
 *
 * @param sector_number  the sector number/count
 *
 * @return the block number/count
 **/
static inline sector_t sector_to_block(sector_t sector_number)
{
	return (sector_number / VDO_SECTORS_PER_BLOCK);
}

/**
 * Convert a sector number to an offset within a block.
 *
 * @param sector_number  the sector number
 *
 * @return the offset within the block
 **/
static inline block_size_t sector_to_block_offset(sector_t sector_number)
{
	unsigned int sectors_per_block_mask = VDO_SECTORS_PER_BLOCK - 1;
	return to_bytes(sector_number & sectors_per_block_mask);
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
 * Update bookkeeping for the completion of some number of requests, so that
 * more incoming requests can be accepted.
 *
 * @param vdo    The vdo
 * @param count  The number of completed requests
 **/
void complete_many_requests(struct vdo *vdo, uint32_t count);


#endif /* KERNELLAYER_H */
