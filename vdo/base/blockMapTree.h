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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapTree.h#11 $
 */

#ifndef BLOCK_MAP_TREE_H
#define BLOCK_MAP_TREE_H

#include "constants.h"
#include "types.h"

struct tree_page;

/**
 * Intialize a block_map_tree_zone.
 *
 * @param zone              The block_map_zone of the tree zone to intialize
 * @param layer             The physical layer
 * @param maximumAge        The number of journal blocks before a dirtied page
 *                          is considered old and may be written out
 *
 * @return VDO_SUCCESS or an error
 **/
int initializeTreeZone(struct block_map_zone *zone,
                       PhysicalLayer         *layer,
                       BlockCount             maximumAge)
  __attribute__((warn_unused_result));

/**
 * Clean up a block_map_tree_zone.
 *
 * @param treeZone  The zone to clean up
 **/
void uninitializeBlockMapTreeZone(struct block_map_tree_zone *treeZone);

/**
 * Set the initial dirty period for a tree zone.
 *
 * @param treeZone  The tree zone
 * @param period    The initial dirty period to set
 **/
void setTreeZoneInitialPeriod(struct block_map_tree_zone *treeZone,
                              SequenceNumber              period);

/**
 * Check whether a tree zone is active (i.e. has any active lookups,
 * outstanding I/O, or pending I/O).
 *
 * @param zone  The zone to check
 *
 * @return <code>true</code> if the zone is active
 **/
bool isTreeZoneActive(struct block_map_tree_zone *zone)
  __attribute__((warn_unused_result));

/**
 * Advance the dirty period for a tree zone.
 *
 * @param zone    The block_map_tree_zone to advance
 * @param period  The new dirty period
 **/
void advanceZoneTreePeriod(struct block_map_tree_zone *zone,
                           SequenceNumber              period);

/**
 * Drain the zone trees, i.e. ensure that all I/O is quiesced. If required by
 * the drain type, all dirty block map trees will be written to disk. This
 * method must not be called when lookups are active.
 *
 * @param zone  The BlockMapTreeZone to drain
 **/
void drainZoneTrees(struct block_map_tree_zone *zone);

/**
 * Look up the PBN of the block map page for a data_vio's LBN in the arboreal
 * block map. If necessary, the block map page will be allocated. Also, the
 * ancestors of the block map page will be allocated or loaded if necessary.
 *
 * @param dataVIO  The data_vio requesting the lookup
 **/
void lookupBlockMapPBN(struct data_vio *dataVIO);

/**
 * Find the PBN of a leaf block map page. This method may only be used after
 * all allocated tree pages have been loaded, otherwise, it may give the wrong
 * answer (0).
 *
 * @param map         The block map containing the forest
 * @param pageNumber  The page number of the desired block map page
 *
 * @return The PBN of the page
 **/
PhysicalBlockNumber findBlockMapPagePBN(struct block_map *map,
                                        PageNumber        pageNumber);

/**
 * Write a tree page or indicate that it has been re-dirtied if it is already
 * being written. This method is used when correcting errors in the tree during
 * read-only rebuild.
 *
 * @param page  The page to write
 * @param zone  The tree zone managing the page
 **/
void writeTreePage(struct tree_page *page, struct block_map_tree_zone *zone);

#endif // BLOCK_MAP_TREE_H
