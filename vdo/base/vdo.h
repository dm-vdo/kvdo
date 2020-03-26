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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo.h#6 $
 */

#ifndef VDO_H
#define VDO_H

#include "types.h"

/**
 * Allocate a vdo structure and associate it with its physical layer.
 *
 * @param [in]  layer       The physical layer the vdo sits on
 * @param [out] vdoPtr      A pointer to hold the allocated vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int allocateVDO(PhysicalLayer *layer, struct vdo **vdoPtr)
	__attribute__((warn_unused_result));

/**
 * Construct a vdo structure for use in user space with a synchronous layer.
 *
 * @param [in]  layer   The physical layer the vdo sits on
 * @param [out] vdoPtr  A pointer to hold the allocated vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int makeVDO(PhysicalLayer *layer, struct vdo **vdoPtr)
	__attribute__((warn_unused_result));

/**
 * Destroy a vdo instance.
 *
 * @param vdo  The vdo to destroy
 **/
void destroyVDO(struct vdo *vdo);

/**
 * Destroy a vdo instance, free it, and null out the reference to it.
 *
 * @param vdoPtr  A reference to the vdo to free
 **/
void freeVDO(struct vdo **vdoPtr);

/**
 * Put a vdo into read-only mode and save the read-only state in the super
 * block.
 *
 * @param vdo             The vdo to put into read-only mode
 * @param errorCode       The error which caused the vdo to enter read-only
 *                        mode
 **/
void makeVDOReadOnly(struct vdo *vdo, int errorCode);

/**
 * Set whether compression is enabled in a vdo.
 *
 * @param vdo                The vdo
 * @param enableCompression  Whether to enable compression in vdo
 *
 * @return State of compression before new value is set
 **/
bool setVDOCompressing(struct vdo *vdo, bool enableCompression);

/**
 * Get whether compression is enabled in a vdo.
 *
 * @param vdo  The vdo
 *
 * @return State of compression
 **/
bool getVDOCompressing(struct vdo *vdo);

/**
 * Get the vdo statistics.
 *
 * @param [in]  vdo    The vdo
 * @param [out] stats  The vdo statistics are returned here
 **/
void getVDOStatistics(const struct vdo *vdo, VDOStatistics *stats);

/**
 * Get the number of physical blocks in use by user data.
 *
 * @param vdo  The vdo
 *
 * @return The number of blocks allocated for user data
 **/
BlockCount getPhysicalBlocksAllocated(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the number of unallocated physical blocks.
 *
 * @param vdo  The vdo
 *
 * @return The number of free blocks
 **/
BlockCount getPhysicalBlocksFree(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the number of physical blocks used by vdo metadata.
 *
 * @param vdo  The vdo
 *
 * @return The number of overhead blocks
 **/
BlockCount getPhysicalBlocksOverhead(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the total number of blocks used for the block map.
 *
 * @param vdo  The vdo
 *
 * @return The number of block map blocks
 **/
BlockCount getTotalBlockMapBlocks(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the vdo write policy.
 *
 * @param vdo  The vdo
 *
 * @return The write policy
 **/
WritePolicy getWritePolicy(const struct vdo *vdo);

/**
 * Set the vdo write policy.
 *
 * @param vdo  The vdo
 * @param new  The new write policy
 **/
void setWritePolicy(struct vdo *vdo, WritePolicy new);

/**
 * Get a copy of the load-time configuration of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The load-time configuration of the vdo
 **/
const VDOLoadConfig *getVDOLoadConfig(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the thread config of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The thread config
 **/
const struct thread_config *getThreadConfig(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the configured maximum age of a dirty block map page.
 *
 * @param vdo  The vdo
 *
 * @return The block map era length
 **/
BlockCount getConfiguredBlockMapMaximumAge(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the configured page cache size of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The number of pages for the page cache
 **/
PageCount getConfiguredCacheSize(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the location of the first block of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The location of the first block managed by the vdo
 **/
PhysicalBlockNumber getFirstBlockOffset(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Check whether the vdo was new when it was loaded.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo was new
 **/
bool wasNew(const struct vdo *vdo) __attribute__((warn_unused_result));

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
struct zoned_pbn validateDedupeAdvice(struct vdo *vdo,
				      const struct data_location *advice,
				      LogicalBlockNumber lbn)
	__attribute__((warn_unused_result));

// TEST SUPPORT ONLY BEYOND THIS POINT

/**
 * Dump status information about a vdo to the log for debugging.
 *
 * @param vdo  The vdo to dump
 **/
void dumpVDOStatus(const struct vdo *vdo);

/**
 * Set the VIO tracing flag.
 *
 * @param vdo         The vdo
 * @param vioTracing  Whether VIO tracing is enabled for this device
 **/
void setVDOTracingFlags(struct vdo *vdo, bool vioTracing);

/**
 * Indicate whether VIO tracing is enabled.
 *
 * @param vdo  The vdo
 *
 * @return Whether VIO tracing is enabled
 **/
bool vdoVIOTracingEnabled(const struct vdo *vdo);

/**
 * Indicate whether extent tracing is enabled.
 *
 * @param vdo  The vdo
 *
 * @return Whether extent tracing is enabled
 **/
bool vdoExtentTracingEnabled(const struct vdo *vdo);

#endif /* VDO_H */
