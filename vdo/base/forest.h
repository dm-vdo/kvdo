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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/forest.h#1 $
 */

#ifndef FOREST_H
#define FOREST_H

#include "blockMapTree.h"
#include "types.h"

/**
 * A function to be called for each allocated PBN when traversing the forest.
 *
 * @param pbn         A PBN of a tree node
 * @param completion  The parent completion of the traversal
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int EntryCallback(PhysicalBlockNumber pbn, VDOCompletion *completion);

/**
 * Get a tree from the forest.
 *
 * @param forest  The forest which contains the desired tree
 * @param index   The index of the tree to get
 *
 * @return The requested tree
 **/
BlockMapTree *getTreeFromForest(Forest *forest, RootCount index)
  __attribute__((warn_unused_result));

/**
 * Get the tree page for a given height and page index.
 *
 * @param forest     The forest which holds the page
 * @param tree       The tree which holds the page
 * @param height     The height of the desired page
 * @param pageIndex  The index of the desired page
 *
 * @return The requested page
 **/
TreePage *getTreePageByIndex(Forest       *forest,
                             BlockMapTree *tree,
                             Height        height,
                             PageNumber    pageIndex)
  __attribute__((warn_unused_result));

/**
 * Make a collection of trees for a BlockMap, expanding the existing forest if
 * there is one.
 *
 * @param map      The block map
 * @param entries  The number of entries the block map will hold
 *
 * @return VDO_SUCCESS or an error
 **/
int makeForest(BlockMap *map, BlockCount entries)
  __attribute__((warn_unused_result));

/**
 * Free a forest and all of the segments it contains and NULL out the reference
 * to it.
 *
 * @param forestPtr  A pointer to the forest to free
 **/
void freeForest(Forest **forestPtr);

/**
 * Abandon the unused next forest from a BlockMap.
 *
 * @param map  The block map
 **/
void abandonForest(BlockMap *map);

/**
 * Replace a BlockMap's Forest with the already-prepared larger forest.
 *
 * @param map  The block map
 **/
void replaceForest(BlockMap *map);

/**
 * Walk the entire forest of a block map.
 *
 * @param map            The block map to traverse
 * @param entryCallback  A function to call with the pbn of each allocated node
 *                       in the forest
 * @param parent         The completion to notify on each traversed PBN, and
 *                       when the traversal is complete
 **/
void traverseForest(BlockMap      *map,
                    EntryCallback *entryCallback,
                    VDOCompletion *parent);

/**
 * Compute the approximate number of pages which the forest will allocate in
 * order to map the specified number of logical blocks. This method assumes
 * that the block map is entirely arboreal.
 *
 * @param logicalBlocks  The number of blocks to map
 * @param rootCount      The number of trees in the forest
 *
 * @return A (slight) over-estimate of the total number of possible forest
 *         pages including the leaves
 **/
BlockCount computeForestSize(BlockCount logicalBlocks, RootCount rootCount)
  __attribute__((warn_unused_result));
#endif // FOREST_H
