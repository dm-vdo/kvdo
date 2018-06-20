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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMapTree.h#2 $
 */

#ifndef BLOCK_MAP_TREE_H
#define BLOCK_MAP_TREE_H

#include "constants.h"
#include "types.h"

typedef struct treePage TreePage;

/**
 * Intialize a BlockMapTreeZone.
 *
 * @param zone             The BlockMapZone of the tree zone to intialize
 * @param layer            The physical layer
 * @param readOnlyContext  The read only context for the VDO
 * @param maximumAge       The number of journal blocks before a dirtied page is
 *                         considered old and may be written out
 *
 * @return VDO_SUCCESS or an error
 **/
int initializeTreeZone(BlockMapZone        *zone,
                       PhysicalLayer       *layer,
                       ReadOnlyModeContext *readOnlyContext,
                       BlockCount           maximumAge)
  __attribute__((warn_unused_result));

/**
 * Clean up a BlockMapTreeZone.
 *
 * @param treeZone  The zone to clean up
 **/
void uninitializeBlockMapTreeZone(BlockMapTreeZone *treeZone);

/**
 * Set the initial dirty period for a tree zone.
 *
 * @param treeZone  The tree zone
 * @param period    The initial dirty period to set
 **/
void setTreeZoneInitialPeriod(BlockMapTreeZone *treeZone,
                              SequenceNumber    period);

/**
 * Advance the dirty period for a tree zone.
 *
 * @param zone    The BlockMapTreeZone to advance
 * @param period  The new dirty period
 **/
void advanceZoneTreePeriod(BlockMapTreeZone *zone, SequenceNumber period);

/**
 * Close the zone trees. This will write out all the dirty tree pages from the
 * zone.
 *
 * @param zone  The BlockMapTreeZone whose dirty tree pages are to be written
 **/
void closeZoneTrees(BlockMapTreeZone *zone);

/**
 * Suspend a zone's trees by waiting for all outstanding I/O to complete and
 * preventing any new I/O from launching.
 *
 * @param zone        The BlockMapTreeZone to be suspended
 * @param completion  The object to notify once the suspend is complete
 **/
void suspendZoneTrees(BlockMapTreeZone *zone, VDOCompletion *completion);

/**
 * Resume a zone's trees from the suspended state.
 *
 * @param zone  The BlockMapTreeZone to resume
 **/
void resumeZoneTrees(BlockMapTreeZone *zone);

/**
 * Look up the PBN of the block map page for a DataVIO's LBN in the arboreal
 * block map. If necessary, the block map page will be allocated. Also, the
 * ancestors of the block map page will be allocated or loaded if necessary.
 *
 * @param dataVIO  The DataVIO requesting the lookup
 **/
void lookupBlockMapPBN(DataVIO *dataVIO);

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
PhysicalBlockNumber findBlockMapPagePBN(BlockMap *map, PageNumber pageNumber);

/**
 * Write a tree page or indicate that it has been re-dirtied if it is already
 * being written. This method is used when correcting errors in the tree during
 * read-only rebuild.
 *
 * @param page  The page to write
 * @param zone  The tree zone managing the page
 **/
void writeTreePage(TreePage *page, BlockMapTreeZone *zone);

#endif // BLOCK_MAP_TREE_H
