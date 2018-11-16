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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/forest.c#5 $
 */

#include "forest.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMap.h"
#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "blockMapTree.h"
#include "blockMapTreeInternals.h"
#include "constants.h"
#include "dataVIO.h"
#include "dirtyLists.h"
#include "forest.h"
#include "numUtils.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "types.h"
#include "vdoInternal.h"
#include "vioPool.h"

enum {
  BLOCK_MAP_VIO_POOL_SIZE = 64,
};

struct forest {
  BlockMap      *map;
  size_t         segments;
  Boundary      *boundaries;
  TreePage     **pages;
  BlockMapTree   trees[];
};

typedef struct {
  PageNumber pageIndex;
  SlotNumber slot;
} CursorLevel;

typedef struct cursors Cursors;

typedef struct {
  Waiter        waiter;
  BlockMapTree *tree;
  Height        height;
  Cursors      *parent;
  Boundary      boundary;
  CursorLevel   levels[BLOCK_MAP_TREE_HEIGHT];
  VIOPoolEntry *vioPoolEntry;
} Cursor;

struct cursors {
  BlockMap         *map;
  BlockMapTreeZone *zone;
  ObjectPool       *pool;
  EntryCallback    *entryCallback;
  VDOCompletion    *parent;
  RootCount         activeRoots;
  Cursor            cursors[];
};

/**********************************************************************/
BlockMapTree *getTreeFromForest(Forest *forest, RootCount index)
{
  return &(forest->trees[index]);
}

/**********************************************************************/
TreePage *getTreePageByIndex(Forest       *forest,
                             BlockMapTree *tree,
                             Height        height,
                             PageNumber    pageIndex)
{
  PageNumber offset = 0;
  for (size_t segment = 0; segment < forest->segments; segment++) {
    PageNumber border = forest->boundaries[segment].levels[height - 1];
    if (pageIndex < border) {
      return &(tree->segments[segment].levels[height - 1][pageIndex - offset]);
    }
    offset = border;
  }

  return NULL;
}

/**
 * Compute the number of pages which must be allocated at each level in order
 * to grow the forest to a new number of entries.
 *
 * @param [in]  rootCount      The number of roots
 * @param [in]  flatPageCount  The number of flat block map pages
 * @param [in]  oldSizes       The current size of the forest at each level
 * @param [in]  entries        The new number of entries the block map must
 *                             address
 * @param [out] newSizes       The new size of the forest at each level
 *
 * @return The total number of non-leaf pages required
 **/
static BlockCount computeNewPages(RootCount   rootCount,
                                  BlockCount  flatPageCount,
                                  Boundary   *oldSizes,
                                  BlockCount  entries,
                                  Boundary   *newSizes)
{
  PageCount leafPages
    = maxPageCount(computeBlockMapPageCount(entries) - flatPageCount, 1);
  PageCount  levelSize  = computeBucketCount(leafPages, rootCount);
  BlockCount totalPages = 0;
  for (Height height = 0; height < BLOCK_MAP_TREE_HEIGHT; height++) {
    levelSize = computeBucketCount(levelSize, BLOCK_MAP_ENTRIES_PER_PAGE);
    newSizes->levels[height] = levelSize;
    BlockCount newPages = levelSize;
    if (oldSizes != NULL) {
      newPages -= oldSizes->levels[height];
    }
    totalPages += (newPages * rootCount);
  }

  return totalPages;
}

/**********************************************************************/
static int makeSegment(Forest      *oldForest,
                       BlockCount   newPages,
                       Boundary    *newBoundary,
                       Forest      *forest)
{
  size_t index     = (oldForest == NULL) ? 0 : oldForest->segments;
  forest->segments = index + 1;

  int result = ALLOCATE(forest->segments, Boundary, "forest boundary array",
                        &forest->boundaries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(forest->segments, TreePage *, "forest page pointers",
                    &forest->pages);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(newPages, TreePage, "new forest pages",
                    &forest->pages[index]);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (index > 0) {
    memcpy(forest->boundaries, oldForest->boundaries,
           index * sizeof(Boundary));
    memcpy(forest->pages, oldForest->pages, index * sizeof(TreePage *));
  }

  memcpy(&(forest->boundaries[index]), newBoundary, sizeof(Boundary));

  PageCount segmentSizes[BLOCK_MAP_TREE_HEIGHT];
  for (Height height = 0; height < BLOCK_MAP_TREE_HEIGHT; height++) {
    segmentSizes[height] = newBoundary->levels[height];
    if (index > 0) {
      segmentSizes[height] -= oldForest->boundaries[index - 1].levels[height];
    }
  }

  TreePage *pagePtr = forest->pages[index];
  for (RootCount root = 0; root < forest->map->rootCount; root++) {
    BlockMapTree *tree = &(forest->trees[root]);
    int result = ALLOCATE(forest->segments, BlockMapTreeSegment,
                          "tree root segments", &tree->segments);
    if (result != VDO_SUCCESS) {
      return result;
    }

    if (index > 0) {
      memcpy(tree->segments, oldForest->trees[root].segments,
             index * sizeof(BlockMapTreeSegment));
    }

    BlockMapTreeSegment *segment = &(tree->segments[index]);
    for (Height height = 0; height < BLOCK_MAP_TREE_HEIGHT; height++) {
      if (segmentSizes[height] == 0) {
        continue;
      }

      segment->levels[height] = pagePtr;
      if (height == (BLOCK_MAP_TREE_HEIGHT - 1)) {
        // Record the root.
        BlockMapPage *page = formatBlockMapPage(pagePtr->pageBuffer,
                                                forest->map->nonce,
                                                INVALID_PBN, true);
        page->entries[0] = packPBN(forest->map->rootOrigin + root,
                                   MAPPING_STATE_UNCOMPRESSED);
      }
      pagePtr += segmentSizes[height];
    }
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
static void deforest(Forest *forest, size_t firstPageSegment)
{
  if (forest->pages != NULL) {
    for (size_t segment = firstPageSegment; segment < forest->segments;
         segment++) {
      FREE(forest->pages[segment]);
    }
    FREE(forest->pages);
  }

  for (RootCount root = 0; root < forest->map->rootCount; root++) {
    BlockMapTree *tree = &(forest->trees[root]);
    FREE(tree->segments);
  }

  FREE(forest->boundaries);
  FREE(forest);
}

/**********************************************************************/
int makeForest(BlockMap *map, BlockCount entries)
{
  STATIC_ASSERT(offsetof(TreePage, waiter) == 0);

  Forest   *oldForest   = map->forest;
  Boundary *oldBoundary = NULL;
  if (oldForest != NULL) {
    oldBoundary = &(oldForest->boundaries[oldForest->segments - 1]);
  }

  Boundary newBoundary;
  BlockCount newPages = computeNewPages(map->rootCount, map->flatPageCount,
                                        oldBoundary, entries, &newBoundary);
  if (newPages == 0) {
    map->nextEntryCount = entries;
    return VDO_SUCCESS;
  }

  Forest *forest;
  int result = ALLOCATE_EXTENDED(Forest, map->rootCount, BlockMapTree,
                                 __func__, &forest);
  if (result != VDO_SUCCESS) {
    return result;
  }

  forest->map = map;
  result = makeSegment(oldForest, newPages, &newBoundary, forest);
  if (result != VDO_SUCCESS) {
    deforest(forest, forest->segments - 1);
    return result;
  }

  map->nextForest     = forest;
  map->nextEntryCount = entries;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeForest(Forest **forestPtr)
{
  Forest *forest = *forestPtr;
  if (forest == NULL) {
    return;
  }

  deforest(forest, 0);
  *forestPtr = NULL;
}

/**********************************************************************/
void abandonForest(BlockMap *map)
{
  Forest *forest = map->nextForest;
  map->nextForest = NULL;
  if (forest != NULL) {
    deforest(forest, forest->segments - 1);
  }

  map->nextEntryCount = 0;
}

/**********************************************************************/
void replaceForest(BlockMap *map)
{
  Forest *oldForest   = map->forest;
  map->forest         = map->nextForest;
  map->nextForest     = NULL;
  map->entryCount     = map->nextEntryCount;
  map->nextEntryCount = 0;

  if (oldForest != NULL) {
    deforest(oldForest, oldForest->segments);
  }
}

/**
 * Finish the traversal of a single tree. If it was the last cursor, finish
 * the traversal.
 *
 * @param cursor  The cursor doing the traversal
 **/
static void finishCursor(Cursor *cursor)
{
  Cursors *cursors = cursor->parent;
  returnVIOToPool(cursors->pool, cursor->vioPoolEntry);
  if (--cursors->activeRoots > 0) {
    return;
  }

  VDOCompletion *parent = cursors->parent;
  FREE(cursors);

  finishCompletion(parent, VDO_SUCCESS);
}

/**********************************************************************/
static void traverse(Cursor *cursor);

/**
 * Continue traversing a block map tree.
 *
 * @param completion  The VIO doing a read or write
 **/
static void continueTraversal(VDOCompletion *completion)
{
  VIOPoolEntry *poolEntry = completion->parent;
  Cursor       *cursor    = poolEntry->parent;
  traverse(cursor);
}

/**
 * Continue traversing a block map tree now that a page has been loaded.
 *
 * @param completion  The VIO doing the read
 **/
static void finishTraversalLoad(VDOCompletion *completion)
{
  VIOPoolEntry *entry  = completion->parent;
  Cursor       *cursor = entry->parent;
  Height        height = cursor->height;
  CursorLevel  *level  = &cursor->levels[height];

  TreePage     *treePage
    = &(cursor->tree->segments[0].levels[height][level->pageIndex]);
  BlockMapPage *page = (BlockMapPage *) treePage->pageBuffer;
  copyValidPage(entry->buffer, cursor->parent->map->nonce,
                entry->vio->physical, page);
  traverse(cursor);
}

/**
 * Traverse a single block map tree. This is the recursive heart of the
 * traversal process.
 *
 * @param cursor  The cursor doing the traversal
 **/
static void traverse(Cursor *cursor)
{
  for (; cursor->height < BLOCK_MAP_TREE_HEIGHT; cursor->height++) {
    Height       height = cursor->height;
    CursorLevel *level  = &cursor->levels[height];
    TreePage *treePage
      = &(cursor->tree->segments[0].levels[height][level->pageIndex]);
    BlockMapPage *page = (BlockMapPage *) treePage->pageBuffer;
    if (!isBlockMapPageInitialized(page)) {
      continue;
    }

    for (; level->slot < BLOCK_MAP_ENTRIES_PER_PAGE; level->slot++) {
      DataLocation location = unpackBlockMapEntry(&page->entries[level->slot]);
      if (!isValidLocation(&location)) {
        // This entry is invalid, so remove it from the page.
        page->entries[level->slot]
          = packPBN(ZERO_BLOCK, MAPPING_STATE_UNMAPPED);
        writeTreePage(treePage, cursor->parent->zone);
        continue;
      }

      if (!isMappedLocation(&location)) {
        continue;
      }

      PageNumber entryIndex
        = (BLOCK_MAP_ENTRIES_PER_PAGE * level->pageIndex) + level->slot;

      // Erase mapped entries past the end of the logical space.
      if (entryIndex >= cursor->boundary.levels[height]) {
        page->entries[level->slot]
          = packPBN(ZERO_BLOCK, MAPPING_STATE_UNMAPPED);
        writeTreePage(treePage, cursor->parent->zone);
        continue;
      }

      if (cursor->height < BLOCK_MAP_TREE_HEIGHT - 1) {
        int result = cursor->parent->entryCallback(location.pbn,
                                                   cursor->parent->parent);
        if (result != VDO_SUCCESS) {
          page->entries[level->slot]
            = packPBN(ZERO_BLOCK, MAPPING_STATE_UNMAPPED);
          writeTreePage(treePage, cursor->parent->zone);
          continue;
        }
      }

      if (cursor->height == 0) {
        continue;
      }

      cursor->height--;
      CursorLevel *nextLevel = &cursor->levels[cursor->height];
      nextLevel->pageIndex   = entryIndex;
      nextLevel->slot        = 0;
      level->slot++;
      launchReadMetadataVIO(cursor->vioPoolEntry->vio, location.pbn,
                            finishTraversalLoad, continueTraversal);
      return;
    }
  }

  finishCursor(cursor);
}

/**
 * Start traversing a single block map tree now that the Cursor has a VIO with
 * which to load pages.
 *
 * <p>Implements WaiterCallback.
 *
 * @param waiter   The Cursor
 * @param context  The VIOPoolEntry just acquired
 **/
static void launchCursor(Waiter *waiter, void *context)
{
  STATIC_ASSERT(offsetof(Cursor, waiter) == 0);
  Cursor *cursor               = (Cursor *) waiter;
  cursor->vioPoolEntry         = (VIOPoolEntry *) context;
  cursor->vioPoolEntry->parent = cursor;
  vioAsCompletion(cursor->vioPoolEntry->vio)->callbackThreadID
    = cursor->parent->zone->mapZone->threadID;
  traverse(cursor);
}

/**
 * Compute the number of pages used at each level of the given root's tree.
 *
 * @param map        The block map
 * @param rootIndex  The index of the root to measure
 *
 * @return The list of page counts as a Boundary
 **/
static Boundary computeBoundary(BlockMap *map, RootCount rootIndex)
{
  PageCount leafPages     = computeBlockMapPageCount(map->entryCount);
  PageCount treeLeafPages = leafPages - map->flatPageCount;

  /*
   * Compute the leaf pages for this root. If the number of leaf pages does
   * not distribute evenly, we must determine if this root gets an extra page.
   * Extra pages are assigned to roots starting at firstTreeRoot and going up.
   */
  PageCount firstTreeRoot = map->flatPageCount % map->rootCount;
  PageCount lastTreeRoot  = (leafPages - 1) % map->rootCount;

  PageCount levelPages = treeLeafPages / map->rootCount;
  if (inCyclicRange(firstTreeRoot, rootIndex, lastTreeRoot, map->rootCount)) {
    levelPages++;
  }

  Boundary boundary;
  for (Height height = 0; height < BLOCK_MAP_TREE_HEIGHT - 1; height++) {
    boundary.levels[height] = levelPages;
    levelPages = computeBucketCount(levelPages, BLOCK_MAP_ENTRIES_PER_PAGE);
  }

  // The root node always exists, even if the root is otherwise unused.
  boundary.levels[BLOCK_MAP_TREE_HEIGHT - 1] = 1;

  return boundary;
}

/**********************************************************************/
void traverseForest(BlockMap      *map,
                    EntryCallback *entryCallback,
                    VDOCompletion *parent)
{
  if (computeBlockMapPageCount(map->entryCount) <= map->flatPageCount) {
    // There are no tree pages, so there's nothing to do.
    finishCompletion(parent, VDO_SUCCESS);
    return;
  }

  Cursors *cursors;
  int result = ALLOCATE_EXTENDED(Cursors, map->rootCount, Cursor, __func__,
                                 &cursors);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  cursors->map           = map;
  cursors->zone          = &(getBlockMapZone(map, 0)->treeZone);
  cursors->pool          = cursors->zone->vioPool;
  cursors->entryCallback = entryCallback;
  cursors->parent        = parent;
  cursors->activeRoots   = map->rootCount;
  for (RootCount root = 0; root < map->rootCount; root++) {
    Cursor *cursor = &cursors->cursors[root];
    *cursor = (Cursor) {
      .tree     = &map->forest->trees[root],
      .height   = BLOCK_MAP_TREE_HEIGHT - 1,
      .parent   = cursors,
      .boundary = computeBoundary(map, root),
    };

    cursor->waiter.callback = launchCursor;
    acquireVIOFromPool(cursors->pool, &cursor->waiter);
  };
}

/**********************************************************************/
BlockCount computeForestSize(BlockCount logicalBlocks, RootCount rootCount)
{
  Boundary newSizes;
  BlockCount approximateNonLeaves
    = computeNewPages(rootCount, 0, NULL, logicalBlocks, &newSizes);

  // Exclude the tree roots since those aren't allocated from slabs,
  // and also exclude the super-roots, which only exist in memory.
  approximateNonLeaves
    -= rootCount * (newSizes.levels[BLOCK_MAP_TREE_HEIGHT - 2]
                    + newSizes.levels[BLOCK_MAP_TREE_HEIGHT - 1]);

  BlockCount approximateLeaves
    = computeBlockMapPageCount(logicalBlocks - approximateNonLeaves);

  // This can be a slight over-estimate since the tree will never have to
  // address these blocks, so it might be a tiny bit smaller.
  return (approximateNonLeaves + approximateLeaves);
}
