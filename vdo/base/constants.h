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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/constants.h#2 $
 */

#ifndef CONSTANTS_H
#define CONSTANTS_H

#include "types.h"

enum {
  /** The number of entries on a block map page */
  BLOCK_MAP_ENTRIES_PER_PAGE                       = 812,

  /** The origin of the flat portion of the block map */
  BLOCK_MAP_FLAT_PAGE_ORIGIN                       = 1,

  /**
   * The height of a block map tree. Assuming a root count of 60 and 812
   * entries per page, this is big enough to represent almost 95 PB of logical
   * space.
   **/
  BLOCK_MAP_TREE_HEIGHT                            = 5,

  /** The number of trees in the arboreal block map */
  DEFAULT_BLOCK_MAP_TREE_ROOT_COUNT                = 60,

  /** The default size of the recovery journal, in blocks */
  DEFAULT_RECOVERY_JOURNAL_SIZE                    = 32 * 1024,

  /** The default size of each slab journal, in blocks */
  DEFAULT_SLAB_JOURNAL_SIZE                        = 224,

  /**
   * The initial size of lbnOperations and pbnOperations, which is based
   * upon the expected maximum number of outstanding VIOs. This value was
   * chosen to make it highly unlikely that the maps would need to be resized.
   **/
  LOCK_MAP_CAPACITY                                = 10000,

  /** The maximum number of logical zones */
  MAX_LOGICAL_ZONES                                = 60,

  /** The maximum number of physical zones */
  MAX_PHYSICAL_ZONES                               = 16,

  /** The base-2 logarithm of the maximum blocks in one slab */
  MAX_SLAB_BITS                                    = 23,

  /** The maximum number of slabs the slab depot supports */
  MAX_SLABS                                        = 8192,

  /**
   * The maximum number of block map pages to load simultaneously during
   * recovery or rebuild.
   **/
  MAXIMUM_SIMULTANEOUS_BLOCK_MAP_RESTORATION_READS = 1024,

  /** The maximum number of VIOs in the system at once */
  MAXIMUM_USER_VIOS                                = 2048,

  /**
   * The number of in-memory recovery journal blocks is determined by:
   * -- 311 journal entries in a 4k block
   * -- maximum of 2048 VIOs making entries at once
   * so we need at least 2048 / 312 = 7 journal blocks.
   **/
  RECOVERY_JOURNAL_TAIL_BUFFER_SIZE                = 64,

  /** The number of sectors per block */
  SECTORS_PER_BLOCK                                = 8,

  /** The only physical block size supported by VDO */
  VDO_BLOCK_SIZE                                   = 4096,

  /** The size of a sector that will not be torn */
  VDO_SECTOR_SIZE                                  = 512,

  /** The physical block number reserved for storing the zero block */
  ZERO_BLOCK                                       = 0,
};

/** The maximum logical space is 4 petabytes, which is 1 terablock. */
extern const BlockCount MAXIMUM_LOGICAL_BLOCKS;

/** The maximum physical space is 256 terabytes, which is 64 gigablocks. */
 extern const BlockCount MAXIMUM_PHYSICAL_BLOCKS;

// unit test minimum
extern const BlockCount MINIMUM_SLAB_JOURNAL_BLOCKS;

#endif // CONSTANTS_H
