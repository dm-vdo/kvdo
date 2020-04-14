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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo.h#11 $
 */

#ifndef VDO_H
#define VDO_H

#include "types.h"

/**
 * Allocate a vdo structure and associate it with its physical layer.
 *
 * @param [in]  layer        The physical layer the vdo sits on
 * @param [out] vdo_ptr      A pointer to hold the allocated vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int allocate_vdo(PhysicalLayer *layer, struct vdo **vdo_ptr)
	__attribute__((warn_unused_result));

/**
 * Construct a vdo structure for use in user space with a synchronous layer.
 *
 * @param [in]  layer    The physical layer the vdo sits on
 * @param [out] vdo_ptr  A pointer to hold the allocated vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int make_vdo(PhysicalLayer *layer, struct vdo **vdo_ptr)
	__attribute__((warn_unused_result));

/**
 * Destroy a vdo instance.
 *
 * @param vdo  The vdo to destroy
 **/
void destroy_vdo(struct vdo *vdo);

/**
 * Destroy a vdo instance, free it, and null out the reference to it.
 *
 * @param vdo_ptr  A reference to the vdo to free
 **/
void free_vdo(struct vdo **vdo_ptr);

/**
 * Put a vdo into read-only mode and save the read-only state in the super
 * block.
 *
 * @param vdo              The vdo to put into read-only mode
 * @param error_code       The error which caused the vdo to enter read-only
 *                         mode
 **/
void make_vdo_read_only(struct vdo *vdo, int error_code);

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
void get_vdo_statistics(const struct vdo *vdo,
			struct vdo_statistics *stats);

/**
 * Get the number of physical blocks in use by user data.
 *
 * @param vdo  The vdo
 *
 * @return The number of blocks allocated for user data
 **/
block_count_t get_physical_blocks_allocated(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the number of unallocated physical blocks.
 *
 * @param vdo  The vdo
 *
 * @return The number of free blocks
 **/
block_count_t get_physical_blocks_free(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the number of physical blocks used by vdo metadata.
 *
 * @param vdo  The vdo
 *
 * @return The number of overhead blocks
 **/
block_count_t get_physical_blocks_overhead(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the total number of blocks used for the block map.
 *
 * @param vdo  The vdo
 *
 * @return The number of block map blocks
 **/
block_count_t get_total_block_map_blocks(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the vdo write policy.
 *
 * @param vdo  The vdo
 *
 * @return The write policy
 **/
write_policy get_write_policy(const struct vdo *vdo);

/**
 * Set the vdo write policy.
 *
 * @param vdo  The vdo
 * @param new  The new write policy
 **/
void set_write_policy(struct vdo *vdo, write_policy new);

/**
 * Get a copy of the load-time configuration of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The load-time configuration of the vdo
 **/
const struct vdo_load_config *get_vdo_load_config(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the thread config of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The thread config
 **/
const struct thread_config *get_thread_config(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the configured maximum age of a dirty block map page.
 *
 * @param vdo  The vdo
 *
 * @return The block map era length
 **/
block_count_t get_configured_block_map_maximum_age(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the configured page cache size of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The number of pages for the page cache
 **/
PageCount get_configured_cache_size(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the location of the first block of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The location of the first block managed by the vdo
 **/
PhysicalBlockNumber get_first_block_offset(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Check whether the vdo was new when it was loaded.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo was new
 **/
bool was_new(const struct vdo *vdo) __attribute__((warn_unused_result));

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
struct zoned_pbn validate_dedupe_advice(struct vdo *vdo,
					const struct data_location *advice,
					LogicalBlockNumber lbn)
	__attribute__((warn_unused_result));

// TEST SUPPORT ONLY BEYOND THIS POINT

/**
 * Dump status information about a vdo to the log for debugging.
 *
 * @param vdo  The vdo to dump
 **/
void dump_vdo_status(const struct vdo *vdo);

/**
 * Set the VIO tracing flag.
 *
 * @param vdo          The vdo
 * @param vio_tracing  Whether VIO tracing is enabled for this device
 **/
void set_vdo_tracing_flags(struct vdo *vdo, bool vio_tracing);

/**
 * Indicate whether VIO tracing is enabled.
 *
 * @param vdo  The vdo
 *
 * @return Whether VIO tracing is enabled
 **/
bool vdo_vio_tracing_enabled(const struct vdo *vdo);

/**
 * Indicate whether extent tracing is enabled.
 *
 * @param vdo  The vdo
 *
 * @return Whether extent tracing is enabled
 **/
bool vdo_extent_tracing_enabled(const struct vdo *vdo);

#endif /* VDO_H */
