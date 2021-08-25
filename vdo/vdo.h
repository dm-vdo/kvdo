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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo.h#46 $
 */

#ifndef VDO_H
#define VDO_H

#include <linux/blk_types.h>

#include "deviceConfig.h"
#include "types.h"

/**
 * Destroy a vdo instance.
 *
 * @param vdo  The vdo to destroy
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
 * Get the number of physical blocks in use by user data.
 *
 * @param vdo  The vdo
 *
 * @return The number of blocks allocated for user data
 **/
block_count_t __must_check
get_vdo_physical_blocks_allocated(const struct vdo *vdo);

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
