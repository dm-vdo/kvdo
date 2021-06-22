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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo.h#36 $
 */

#ifndef VDO_H
#define VDO_H

#include <linux/blk_types.h>

#include "types.h"

/**
 * Destroy a vdo instance.
 *
 * @param vdo  The vdo to destroy
 **/
void destroy_vdo(struct vdo *vdo);

/**
 * Wait until there are no requests in progress.
 *
 * @param vdo  The vdo on which to wait
 **/
void vdo_wait_for_no_requests_active(struct vdo *vdo);

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
 * @param layer The kernel layer
 *
 * @return VDO_SUCCESS or an error
 */
int __must_check vdo_synchronous_flush(struct vdo *vdo);

/**
 * Set whether compression is enabled in a vdo.
 *
 * @param vdo                 The vdo
 * @param enable_compression  Whether to enable compression in vdo
 *
 * @return State of compression before new value is set
 **/
bool set_vdo_compressing(struct vdo *vdo, bool enable_compression);

/**
 * Get whether compression is enabled in a vdo.
 *
 * @param vdo  The vdo
 *
 * @return State of compression
 **/
bool get_vdo_compressing(struct vdo *vdo);

/**
 * Get the vdo statistics.
 *
 * @param [in]  vdo    The vdo
 * @param [out] stats  The vdo statistics are returned here
 **/
void get_vdo_statistics(const struct vdo *vdo, struct vdo_statistics *stats);

/**
 * Get the number of physical blocks in use by user data.
 *
 * @param vdo  The vdo
 *
 * @return The number of blocks allocated for user data
 **/
block_count_t __must_check get_vdo_physical_blocks_allocated(const struct vdo *vdo);

/**
 * Get the number of unallocated physical blocks.
 *
 * @param vdo  The vdo
 *
 * @return The number of free blocks
 **/
block_count_t __must_check get_vdo_physical_blocks_free(const struct vdo *vdo);

/**
 * Get the number of physical blocks used by vdo metadata.
 *
 * @param vdo  The vdo
 *
 * @return The number of overhead blocks
 **/
block_count_t __must_check get_vdo_physical_blocks_overhead(const struct vdo *vdo);

/**
 * Get the number of physical blocks in a vdo volume.
 *
 * @param vdo  The vdo
 *
 * @return The physical block count of the vdo
 **/
block_count_t get_vdo_physical_block_count(const struct vdo *vdo);

/**
 * Get a copy of the load-time configuration of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The configuration of the vdo
 **/
const struct device_config * __must_check
get_vdo_device_config(const struct vdo *vdo);

/**
 * Get the thread config of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The thread config
 **/
const struct thread_config * __must_check
get_vdo_thread_config(const struct vdo *vdo);

/**
 * Get the id of the callback thread on which a completion is currently
 * running, or -1 if no such thread.
 *
 * @return the current thread ID
 **/
thread_id_t vdo_get_callback_thread_id(void);

/**
 * Get the configured maximum age of a dirty block map page.
 *
 * @param vdo  The vdo
 *
 * @return The block map era length
 **/
block_count_t __must_check
get_vdo_configured_block_map_maximum_age(const struct vdo *vdo);

/**
 * Get the configured page cache size of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The number of pages for the page cache
 **/
page_count_t __must_check get_vdo_configured_cache_size(const struct vdo *vdo);

/**
 * Get the location of the first block of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The location of the first block managed by the vdo
 **/
physical_block_number_t __must_check
get_vdo_first_block_offset(const struct vdo *vdo);

/**
 * Check whether the vdo was new when it was loaded.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo was new
 **/
bool __must_check vdo_was_new(const struct vdo *vdo);

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

// TEST SUPPORT ONLY BEYOND THIS POINT

/**
 * Dump status information about a vdo to the log for debugging.
 *
 * @param vdo  The vdo to dump
 **/
void dump_vdo_status(const struct vdo *vdo);

#endif /* VDO_H */
