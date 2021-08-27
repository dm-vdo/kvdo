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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelLayer.h#103 $
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
 * Update bookkeeping for the completion of some number of requests, so that
 * more incoming requests can be accepted.
 *
 * @param vdo    The vdo
 * @param count  The number of completed requests
 **/
void complete_many_requests(struct vdo *vdo, uint32_t count);


#endif /* KERNELLAYER_H */
