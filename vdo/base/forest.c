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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/forest.c#15 $
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
#include "dirtyLists.h"
#include "forest.h"
#include "numUtils.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "types.h"
#include "vdoInternal.h"
#include "vio.h"
#include "vioPool.h"

enum {
  BLOCK_MAP_VIO_POOL_SIZE = 64,
};

struct block_map_tree_segment {
  struct tree_page *levels[BLOCK_MAP_TREE_HEIGHT];
};

struct block_map_tree {
  struct block_map_tree_segment *segments;
} block_map_tree;

struct forest {
  struct block_map       *map;
  size_t                  segments;
  struct boundary        *boundaries;
  struct tree_page      **pages;
  struct block_map_tree   trees[];
};

struct cursor_level {
  PageNumber pageIndex;
  SlotNumber slot;
};

struct cursors;

struct cursor {
  struct waiter          waiter;
  struct block_map_tree *tree;
  Height                 height;
  struct cursors        *parent;
  struct boundary        boundary;
  struct cursor_level    levels[BLOCK_MAP_TREE_HEIGHT];
  struct vio_pool_entry *vioPoolEntry;
};

struct cursors {
  struct block_map           *map;
  struct block_map_tree_zone *zone;
  struct vio_pool            *pool;
  EntryCallback              *entryCallback;
  struct vdo_completion      *parent;
  RootCount                   activeRoots;
  struct cursor               cursors[];
};

/**********************************************************************/
struct tree_page *getTreePageByIndex(struct forest *forest,
                                     RootCount      rootIndex,
                                     Height         height,
                                     PageNumber     pageIndex)
{
  PageNumber offset = 0;
  for (size_t segment = 0; segment < forest->segments; segment++) {
    PageNumber border = forest->boundaries[segment].levels[height - 1];
    if (pageIndex < border) {
      struct block_map_tree *tree = &forest->trees[rootIndex];
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
static BlockCount computeNewPages(RootCount          rootCount,
                                  BlockCount         flatPageCount,
                                  struct boundary   *oldSizes,
                                  BlockCount         entries,
                                  struct boundary   *newSizes)
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
static int makeSegment(struct forest   *oldForest,
                       BlockCount       newPages,
                       struct boundary *newBoundary,
                       struct forest   *forest)
{
  size_t index     = (oldForest == NULL) ? 0 : oldForest->segments;
  forest->segments = index + 1;

  int result = ALLOCATE(forest->segments, struct boundary,
                        "forest boundary array", &forest->boundaries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(forest->segments, struct tree_page *,
                    "forest page pointers",
                    &forest->pages);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(newPages, struct tree_page, "new forest pages",
                    &forest->pages[index]);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (index > 0) {
    memcpy(forest->boundaries, oldForest->boundaries,
           index * sizeof(struct boundary));
    memcpy(forest->pages, oldForest->pages, index * sizeof(struct tree_page *));
  }

  memcpy(&(forest->boundaries[index]), newBoundary, sizeof(struct boundary));

  PageCount segmentSizes[BLOCK_MAP_TREE_HEIGHT];
  for (Height height = 0; height < BLOCK_MAP_TREE_HEIGHT; height++) {
    segmentSizes[height] = newBoundary->levels[height];
    if (index > 0) {
      segmentSizes[height] -= oldForest->boundaries[index - 1].levels[height];
    }
  }

  struct tree_page *pagePtr = forest->pages[index];
  for (RootCount root = 0; root < forest->map->rootCount; root++) {
    struct block_map_tree *tree = &(forest->trees[root]);
    int result = ALLOCATE(forest->segments, struct block_map_tree_segment,
                          "tree root segments", &tree->segments);
    if (result != VDO_SUCCESS) {
      return result;
    }

    if (index > 0) {
      memcpy(tree->segments, oldForest->trees[root].segments,
             index * sizeof(struct block_map_tree_segment));
    }

    struct block_map_tree_segment *segment = &(tree->segments[index]);
    for (Height height = 0; height < BLOCK_MAP_TREE_HEIGHT; height++) {
      if (segmentSizes[height] == 0) {
        continue;
      }

      segment->levels[height] = pagePtr;
      if (height == (BLOCK_MAP_TREE_HEIGHT - 1)) {
        // Record the root.
        struct block_map_page *page = formatBlockMapPage(pagePtr->pageBuffer,
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
static void deforest(struct forest *forest, size_t firstPageSegment)
{
  if (forest->pages != NULL) {
    for (size_t segment = firstPageSegment; segment < forest->segments;
         segment++) {
      FREE(forest->pages[segment]);
    }
    FREE(forest->pages);
  }

  for (RootCount root = 0; root < forest->map->rootCount; root++) {
    struct block_map_tree *tree = &(forest->trees[root]);
    FREE(tree->segments);
  }

  FREE(forest->boundaries);
  FREE(forest);
}

/**********************************************************************/
int makeForest(struct block_map *map, BlockCount entries)
{
  STATIC_ASSERT(offsetof(struct tree_page, waiter) == 0);

  struct forest   *oldForest   = map->forest;
  struct boundary *oldBoundary = NULL;
  if (oldForest != NULL) {
    oldBoundary = &(oldForest->boundaries[oldForest->segments - 1]);
  }

  struct boundary newBoundary;
  BlockCount newPages = computeNewPages(map->rootCount, map->flatPageCount,
                                        oldBoundary, entries, &newBoundary);
  if (newPages == 0) {
    map->nextEntryCount = entries;
    return VDO_SUCCESS;
  }

  struct forest *forest;
  int result = ALLOCATE_EXTENDED(struct forest, map->rootCount,
                                 struct block_map_tree, __func__, &forest);
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
void freeForest(struct forest **forestPtr)
{
  struct forest *forest = *forestPtr;
  if (forest == NULL) {
    return;
  }

  deforest(forest, 0);
  *forestPtr = NULL;
}

/**********************************************************************/
void abandonForest(struct block_map *map)
{
  struct forest *forest = map->nextForest;
  map->nextForest = NULL;
  if (forest != NULL) {
    deforest(forest, forest->segments - 1);
  }

  map->nextEntryCount = 0;
}

/**********************************************************************/
void replaceForest(struct block_map *map)
{
  if (map->nextForest != NULL) {
    if (map->forest != NULL) {
      deforest(map->forest, map->forest->segments);
    }
    map->forest     = map->nextForest;
    map->nextForest = NULL;
  }

  map->entryCount     = map->nextEntryCount;
  map->nextEntryCount = 0;
}

/**
 * Finish the traversal of a single tree. If it was the last cursor, finish
 * the traversal.
 *
 * @param cursor  The cursor doing the traversal
 **/
static void finishCursor(struct cursor *cursor)
{
  struct cursors *cursors = cursor->parent;
  returnVIOToPool(cursors->pool, cursor->vioPoolEntry);
  if (--cursors->activeRoots > 0) {
    return;
  }

  struct vdo_completion *parent = cursors->parent;
  FREE(cursors);

  finishCompletion(parent, VDO_SUCCESS);
}

/**********************************************************************/
static void traverse(struct cursor *cursor);

/**
 * Continue traversing a block map tree.
 *
 * @param completion  The VIO doing a read or write
 **/
static void continueTraversal(struct vdo_completion *completion)
{
  struct vio_pool_entry  *poolEntry = completion->parent;
  struct cursor          *cursor    = poolEntry->parent;
  traverse(cursor);
}

/**
 * Continue traversing a block map tree now that a page has been loaded.
 *
 * @param completion  The VIO doing the read
 **/
static void finishTraversalLoad(struct vdo_completion *completion)
{
  struct vio_pool_entry         *entry  = completion->parent;
  struct cursor                 *cursor = entry->parent;
  Height                         height = cursor->height;
  struct cursor_level           *level  = &cursor->levels[height];

  struct tree_page *treePage
    = &(cursor->tree->segments[0].levels[height][level->pageIndex]);
  struct block_map_page *page = (struct block_map_page *) treePage->pageBuffer;
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
static void traverse(struct cursor *cursor)
{
  for (; cursor->height < BLOCK_MAP_TREE_HEIGHT; cursor->height++) {
    Height               height = cursor->height;
    struct cursor_level *level  = &cursor->levels[height];
    struct tree_page *treePage
      = &(cursor->tree->segments[0].levels[height][level->pageIndex]);
    struct block_map_page *page
      = (struct block_map_page *) treePage->pageBuffer;
    if (!isBlockMapPageInitialized(page)) {
      continue;
    }

    for (; level->slot < BLOCK_MAP_ENTRIES_PER_PAGE; level->slot++) {
      struct data_location location
        = unpackBlockMapEntry(&page->entries[level->slot]);
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
      struct cursor_level *nextLevel = &cursor->levels[cursor->height];
      nextLevel->pageIndex           = entryIndex;
      nextLevel->slot                = 0;
      level->slot++;
      launchReadMetadataVIO(cursor->vioPoolEntry->vio, location.pbn,
                            finishTraversalLoad, continueTraversal);
      return;
    }
  }

  finishCursor(cursor);
}

/**
 * Start traversing a single block map tree now that the cursor has a VIO with
 * which to load pages.
 *
 * <p>Implements WaiterCallback.
 *
 * @param waiter   The cursor
 * @param context  The vio_pool_entry just acquired
 **/
static void launchCursor(struct waiter *waiter, void *context)
{
  STATIC_ASSERT(offsetof(struct cursor, waiter) == 0);
  struct cursor *cursor               = (struct cursor *) waiter;
  cursor->vioPoolEntry                = (struct vio_pool_entry *) context;
  cursor->vioPoolEntry->parent        = cursor;
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
 * @return The list of page counts as a boundary structure
 **/
static struct boundary computeBoundary(struct block_map *map,
                                       RootCount         rootIndex)
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

  struct boundary boundary;
  for (Height height = 0; height < BLOCK_MAP_TREE_HEIGHT - 1; height++) {
    boundary.levels[height] = levelPages;
    levelPages = computeBucketCount(levelPages, BLOCK_MAP_ENTRIES_PER_PAGE);
  }

  // The root node always exists, even if the root is otherwise unused.
  boundary.levels[BLOCK_MAP_TREE_HEIGHT - 1] = 1;

  return boundary;
}

/**********************************************************************/
void traverseForest(struct block_map      *map,
                    EntryCallback         *entryCallback,
                    struct vdo_completion *parent)
{
  if (computeBlockMapPageCount(map->entryCount) <= map->flatPageCount) {
    // There are no tree pages, so there's nothing to do.
    finishCompletion(parent, VDO_SUCCESS);
    return;
  }

  struct cursors *cursors;
  int result = ALLOCATE_EXTENDED(struct cursors, map->rootCount,
                                 struct cursor, __func__, &cursors);
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
    struct cursor *cursor = &cursors->cursors[root];
    *cursor = (struct cursor) {
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
  struct boundary newSizes;
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
