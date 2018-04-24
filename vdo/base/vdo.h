/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdo.h#2 $
 */

#ifndef VDO_H
#define VDO_H

#include "types.h"

/**
 * Allocate a VDO and associate it with its physical layer.
 *
 * @param [in]  layer       The physical layer the VDO sits on
 * @param [out] vdoPtr      A pointer to hold the allocated VDO
 *
 * @return VDO_SUCCESS or an error
 **/
int allocateVDO(PhysicalLayer *layer, VDO **vdoPtr)
  __attribute__((warn_unused_result));

/**
 * Construct a VDO for use in user space with a synchronous layer.
 *
 * @param [in]  layer   The physical layer the VDO sits on
 * @param [out] vdoPtr  A pointer to hold the allocated VDO
 *
 * @return VDO_SUCCESS or an error
 **/
int makeVDO(PhysicalLayer *layer, VDO **vdoPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy a VDO instance.
 *
 * @param vdo  The VDO to destroy
 **/
void destroyVDO(VDO *vdo);

/**
 * Destroy a VDO instance, free it, and null out the reference to it.
 *
 * @param vdoPtr  A reference to the VDO to free
 **/
void freeVDO(VDO **vdoPtr);

/**
 * Set whether compression is enabled in VDO.
 *
 * @param vdo                The VDO
 * @param enableCompression  Whether to enable compression in VDO
 *
 * @return State of compression before new value is set
 **/
bool setVDOCompressing(VDO *vdo, bool enableCompression);

/**
 * Get whether compression is enabled in VDO.
 *
 * @param vdo  The VDO
 *
 * @return State of compression
 **/
bool getVDOCompressing(VDO *vdo);

/**
 * Get the VDO statistics.
 *
 * @param [in]  vdo    The VDO
 * @param [out] stats  The VDO statistics are returned here
 **/
void getVDOStatistics(const VDO *vdo, VDOStatistics *stats);

/**
 * Get the number of physical blocks in use by user data.
 *
 * @param vdo  The VDO
 *
 * @return The number of blocks allocated for user data
 **/
BlockCount getPhysicalBlocksAllocated(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the number of unallocated physical blocks.
 *
 * @param vdo  The VDO
 *
 * @return The number of free blocks
 **/
BlockCount getPhysicalBlocksFree(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the number of physical blocks used by VDO metadata.
 *
 * @param vdo  The VDO
 *
 * @return The number of overhead blocks
 **/
BlockCount getPhysicalBlocksOverhead(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the total number of blocks used for the block map.
 *
 * @param vdo  The VDO
 *
 * @return The number of block map blocks
 **/
BlockCount getTotalBlockMapBlocks(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the VDO write policy.
 *
 * @param vdo  The VDO
 *
 * @return The write policy
 **/
WritePolicy getWritePolicy(const VDO *vdo);

/**
 * Set the VDO write policy.
 *
 * @param vdo  The VDO
 * @param new  The new write policy
 **/
void setWritePolicy(VDO *vdo, WritePolicy new);

/**
 * Get a copy of the load-time configuration of the VDO.
 *
 * @param vdo  The VDO
 *
 * @return The load-time configuration of the VDO
 **/
const VDOLoadConfig *getVDOLoadConfig(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the thread config of the VDO.
 *
 * @param vdo  The VDO
 *
 * @return The thread config
 **/
const ThreadConfig *getThreadConfig(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the configured maximum age of a dirty block map page.
 *
 * @param vdo  The VDO
 *
 * @return The block map era length
 **/
BlockCount getConfiguredBlockMapMaximumAge(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the configured page cache size of the VDO.
 *
 * @param vdo  The VDO
 *
 * @return The number of pages for the page cache
 **/
PageCount getConfiguredCacheSize(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the location of the first block of the VDO.
 *
 * @param vdo  The VDO
 *
 * @return The location of the first block managed by the VDO
 **/
PhysicalBlockNumber getFirstBlockOffset(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether the VDO was new when it was loaded.
 *
 * @param vdo  The VDO to query
 *
 * @return <code>true</code> if the VDO was new
 **/
bool wasNew(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether a DataLocation containing potential dedupe advice is
 * well-formed and addresses a data block in one of the configured physical
 * zones of the VDO. If it is, return the location and zone as a ZonedPBN;
 * otherwise increment statistics tracking invalid advice and return an
 * unmapped ZonedPBN.
 *
 * @param vdo     The VDO
 * @param advice  The advice to validate (NULL indicates no advice)
 * @param lbn     The logical block number of the write that requested advice,
 *                which is only used for debug-level logging of invalid advice
 *
 * @return The ZonedPBN representing the advice, if valid, otherwise an
 *         unmapped ZonedPBN if the advice was invalid or NULL
 **/
ZonedPBN validateDedupeAdvice(VDO                *vdo,
                              const DataLocation *advice,
                              LogicalBlockNumber  lbn)
  __attribute__((warn_unused_result));

// TEST SUPPORT ONLY BEYOND THIS POINT

/**
 * Dump status information about VDO to the log for debugging.
 *
 * @param vdo  The vdo to dump
 **/
void dumpVDOStatus(const VDO *vdo);

/**
 * Set the VIO tracing flag.
 *
 * @param vdo         The VDO
 * @param vioTracing  Whether VIO tracing is enabled for this device
 **/
void setVDOTracingFlags(VDO *vdo, bool vioTracing);

/**
 * Indicate whether VIO tracing is enabled.
 *
 * @param vdo  The VDO
 *
 * @return Whether VIO tracing is enabled
 **/
bool vdoVIOTracingEnabled(const VDO *vdo);

/**
 * Indicate whether extent tracing is enabled.
 *
 * @param vdo  The VDO
 *
 * @return Whether extent tracing is enabled
 **/
bool vdoExtentTracingEnabled(const VDO *vdo);

#endif /* VDO_H */
