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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/forest.c#37 $
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
	struct block_map *map;
	size_t segments;
	struct boundary *boundaries;
	struct tree_page **pages;
	struct block_map_tree trees[];
};

struct cursor_level {
	page_number_t page_index;
	slot_number_t slot;
};

struct cursors;

struct cursor {
	struct waiter waiter;
	struct block_map_tree *tree;
	height_t height;
	struct cursors *parent;
	struct boundary boundary;
	struct cursor_level levels[BLOCK_MAP_TREE_HEIGHT];
	struct vio_pool_entry *vio_pool_entry;
};

struct cursors {
	struct block_map *map;
	struct block_map_tree_zone *zone;
	struct vio_pool *pool;
	entry_callback *entry_callback;
	struct vdo_completion *parent;
	root_count_t active_roots;
	struct cursor cursors[];
};

/**********************************************************************/
struct tree_page *get_tree_page_by_index(struct forest *forest,
					 root_count_t root_index,
					 height_t height,
					 page_number_t page_index)
{
	page_number_t offset = 0;
	size_t segment;
	for (segment = 0; segment < forest->segments; segment++) {
		page_number_t border =
			forest->boundaries[segment].levels[height - 1];
		if (page_index < border) {
			struct block_map_tree *tree = &forest->trees[root_index];
			return &(tree->segments[segment]
				 .levels[height - 1][page_index - offset]);
		}
		offset = border;
	}

	return NULL;
}

/**********************************************************************/
static int make_segment(struct forest *old_forest,
			block_count_t new_pages,
			struct boundary *new_boundary,
			struct forest *forest)
{
	size_t index = (old_forest == NULL) ? 0 : old_forest->segments;
	forest->segments = index + 1;

	int result = ALLOCATE(forest->segments,
			      struct boundary,
			      "forest boundary array",
			      &forest->boundaries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ALLOCATE(forest->segments,
			  struct tree_page *,
			  "forest page pointers",
			  &forest->pages);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ALLOCATE(new_pages,
			  struct tree_page,
			  "new forest pages",
			  &forest->pages[index]);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (index > 0) {
		memcpy(forest->boundaries,
		       old_forest->boundaries,
		       index * sizeof(struct boundary));
		memcpy(forest->pages,
		       old_forest->pages,
		       index * sizeof(struct tree_page *));
	}

	memcpy(&(forest->boundaries[index]),
	       new_boundary,
	       sizeof(struct boundary));

	page_count_t segment_sizes[BLOCK_MAP_TREE_HEIGHT];
	height_t height;
	for (height = 0; height < BLOCK_MAP_TREE_HEIGHT; height++) {
		segment_sizes[height] = new_boundary->levels[height];
		if (index > 0) {
			segment_sizes[height] -=
				old_forest->boundaries[index - 1].levels[height];
		}
	}

	struct tree_page *page_ptr = forest->pages[index];
	root_count_t root;
	for (root = 0; root < forest->map->root_count; root++) {
		struct block_map_tree *tree = &(forest->trees[root]);
		int result = ALLOCATE(forest->segments,
				      struct block_map_tree_segment,
				      "tree root segments",
				      &tree->segments);
		if (result != VDO_SUCCESS) {
			return result;
		}

		if (index > 0) {
			memcpy(tree->segments,
			       old_forest->trees[root].segments,
			       index * sizeof(struct block_map_tree_segment));
		}

		struct block_map_tree_segment *segment =
			&(tree->segments[index]);
		height_t height;
		for (height = 0; height < BLOCK_MAP_TREE_HEIGHT; height++) {
			if (segment_sizes[height] == 0) {
				continue;
			}

			segment->levels[height] = page_ptr;
			if (height == (BLOCK_MAP_TREE_HEIGHT - 1)) {
				// Record the root.
				struct block_map_page *page =
					format_block_map_page(page_ptr->page_buffer,
							      forest->map->nonce,
							      INVALID_PBN,
							      true);
				page->entries[0] =
					pack_pbn(forest->map->root_origin + root,
						MAPPING_STATE_UNCOMPRESSED);
			}
			page_ptr += segment_sizes[height];
		}
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
static void deforest(struct forest *forest, size_t first_page_segment)
{
	if (forest->pages != NULL) {
		size_t segment;
		for (segment = first_page_segment; segment < forest->segments;
		     segment++) {
			FREE(forest->pages[segment]);
		}
		FREE(forest->pages);
	}

	root_count_t root;
	for (root = 0; root < forest->map->root_count; root++) {
		struct block_map_tree *tree = &(forest->trees[root]);
		FREE(tree->segments);
	}

	FREE(forest->boundaries);
	FREE(forest);
}

/**********************************************************************/
int make_forest(struct block_map *map, block_count_t entries)
{
	struct forest *old_forest = map->forest;
	struct boundary *old_boundary = NULL;
	if (old_forest != NULL) {
		old_boundary =
			&(old_forest->boundaries[old_forest->segments - 1]);
	}

	struct boundary new_boundary;
	block_count_t new_pages = compute_new_forest_pages(map->root_count,
							   old_boundary,
							   entries,
							   &new_boundary);
	if (new_pages == 0) {
		map->next_entry_count = entries;
		return VDO_SUCCESS;
	}

	struct forest *forest;
	int result = ALLOCATE_EXTENDED(struct forest,
				       map->root_count,
				       struct block_map_tree,
				       __func__,
				       &forest);
	if (result != VDO_SUCCESS) {
		return result;
	}

	forest->map = map;
	result = make_segment(old_forest, new_pages, &new_boundary, forest);
	if (result != VDO_SUCCESS) {
		deforest(forest, forest->segments - 1);
		return result;
	}

	map->next_forest = forest;
	map->next_entry_count = entries;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_forest(struct forest **forest_ptr)
{
	struct forest *forest = *forest_ptr;
	if (forest == NULL) {
		return;
	}

	deforest(forest, 0);
	*forest_ptr = NULL;
}

/**********************************************************************/
void abandon_forest(struct block_map *map)
{
	struct forest *forest = map->next_forest;
	map->next_forest = NULL;
	if (forest != NULL) {
		deforest(forest, forest->segments - 1);
	}

	map->next_entry_count = 0;
}

/**********************************************************************/
void replace_forest(struct block_map *map)
{
	if (map->next_forest != NULL) {
		if (map->forest != NULL) {
			deforest(map->forest, map->forest->segments);
		}
		map->forest = map->next_forest;
		map->next_forest = NULL;
	}

	map->entry_count = map->next_entry_count;
	map->next_entry_count = 0;
}

/**
 * Finish the traversal of a single tree. If it was the last cursor, finish
 * the traversal.
 *
 * @param cursor  The cursor doing the traversal
 **/
static void finish_cursor(struct cursor *cursor)
{
	struct cursors *cursors = cursor->parent;
	return_vio_to_pool(cursors->pool, cursor->vio_pool_entry);
	if (--cursors->active_roots > 0) {
		return;
	}

	struct vdo_completion *parent = cursors->parent;
	FREE(cursors);

	finish_completion(parent, VDO_SUCCESS);
}

/**********************************************************************/
static void traverse(struct cursor *cursor);

/**
 * Continue traversing a block map tree.
 *
 * @param completion  The VIO doing a read or write
 **/
static void continue_traversal(struct vdo_completion *completion)
{
	struct vio_pool_entry *pool_entry = completion->parent;
	struct cursor *cursor = pool_entry->parent;
	traverse(cursor);
}

/**
 * Continue traversing a block map tree now that a page has been loaded.
 *
 * @param completion  The VIO doing the read
 **/
static void finish_traversal_load(struct vdo_completion *completion)
{
	struct vio_pool_entry *entry = completion->parent;
	struct cursor *cursor = entry->parent;
	height_t height = cursor->height;
	struct cursor_level *level = &cursor->levels[height];

	struct tree_page *tree_page =
		&(cursor->tree->segments[0].levels[height][level->page_index]);
	struct block_map_page *page =
		(struct block_map_page *) tree_page->page_buffer;
	copy_valid_page(entry->buffer,
			cursor->parent->map->nonce,
			entry->vio->physical,
			page);
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
		height_t height = cursor->height;
		struct cursor_level *level = &cursor->levels[height];
		struct tree_page *tree_page =
			&(cursor->tree->segments[0]
				  .levels[height][level->page_index]);
		struct block_map_page *page =
			(struct block_map_page *) tree_page->page_buffer;
		if (!is_block_map_page_initialized(page)) {
			continue;
		}

		for (; level->slot < BLOCK_MAP_ENTRIES_PER_PAGE;
		     level->slot++) {
			struct data_location location =
				unpack_block_map_entry(&page->entries[level->slot]);
			if (!is_valid_location(&location)) {
				// This entry is invalid, so remove it from the
				// page.
				page->entries[level->slot] =
					pack_pbn(ZERO_BLOCK,
						 MAPPING_STATE_UNMAPPED);
				write_tree_page(tree_page,
						cursor->parent->zone);
				continue;
			}

			if (!is_mapped_location(&location)) {
				continue;
			}

			page_number_t entry_index =
				(BLOCK_MAP_ENTRIES_PER_PAGE *
				 level->page_index) + level->slot;

			// Erase mapped entries past the end of the logical
			// space.
			if (entry_index >= cursor->boundary.levels[height]) {
				page->entries[level->slot] =
					pack_pbn(ZERO_BLOCK,
						 MAPPING_STATE_UNMAPPED);
				write_tree_page(tree_page,
						cursor->parent->zone);
				continue;
			}

			if (cursor->height < BLOCK_MAP_TREE_HEIGHT - 1) {
				int result =
					cursor->parent->entry_callback(location.pbn,
								       cursor->parent->parent);
				if (result != VDO_SUCCESS) {
					page->entries[level->slot] =
						pack_pbn(ZERO_BLOCK,
							 MAPPING_STATE_UNMAPPED);
					write_tree_page(tree_page,
							cursor->parent->zone);
					continue;
				}
			}

			if (cursor->height == 0) {
				continue;
			}

			cursor->height--;
			struct cursor_level *next_level =
				&cursor->levels[cursor->height];
			next_level->page_index = entry_index;
			next_level->slot = 0;
			level->slot++;
			launch_read_metadata_vio(cursor->vio_pool_entry->vio,
						 location.pbn,
						 finish_traversal_load,
						 continue_traversal);
			return;
		}
	}

	finish_cursor(cursor);
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
static void launch_cursor(struct waiter *waiter, void *context)
{
	struct cursor *cursor = container_of(waiter, struct cursor, waiter);
	cursor->vio_pool_entry = (struct vio_pool_entry *) context;
	cursor->vio_pool_entry->parent = cursor;
	vio_as_completion(cursor->vio_pool_entry->vio)->callback_thread_id =
		cursor->parent->zone->map_zone->thread_id;
	traverse(cursor);
}

/**
 * Compute the number of pages used at each level of the given root's tree.
 *
 * @param map         The block map
 * @param root_index  The index of the root to measure
 *
 * @return The list of page counts as a boundary structure
 **/
static struct boundary compute_boundary(struct block_map *map,
				       root_count_t root_index)
{
	page_count_t leaf_pages =
		compute_block_map_page_count(map->entry_count);

	/*
	 * Compute the leaf pages for this root. If the number of leaf pages
	 * does not distribute evenly, we must determine if this root gets an
	 * extra page. Extra pages are assigned to roots starting from tree 0.
	 */
	page_count_t last_tree_root = (leaf_pages - 1) % map->root_count;
	page_count_t level_pages = leaf_pages / map->root_count;
	if (root_index <= last_tree_root) {
		level_pages++;
	}

	struct boundary boundary;
	height_t height;
	for (height = 0; height < BLOCK_MAP_TREE_HEIGHT - 1; height++) {
		boundary.levels[height] = level_pages;
		level_pages = compute_bucket_count(level_pages,
						   BLOCK_MAP_ENTRIES_PER_PAGE);
	}

	// The root node always exists, even if the root is otherwise unused.
	boundary.levels[BLOCK_MAP_TREE_HEIGHT - 1] = 1;

	return boundary;
}

/**********************************************************************/
void traverse_forest(struct block_map *map,
		     entry_callback *callback,
		     struct vdo_completion *parent)
{
	struct cursors *cursors;
	int result = ALLOCATE_EXTENDED(struct cursors,
				       map->root_count,
				       struct cursor,
				       __func__,
				       &cursors);
	if (result != VDO_SUCCESS) {
		finish_completion(parent, result);
		return;
	}

	cursors->map = map;
	cursors->zone = &(get_block_map_zone(map, 0)->tree_zone);
	cursors->pool = cursors->zone->vio_pool;
	cursors->entry_callback = callback;
	cursors->parent = parent;
	cursors->active_roots = map->root_count;
	root_count_t root;
	for (root = 0; root < map->root_count; root++) {
		struct cursor *cursor = &cursors->cursors[root];
		*cursor = (struct cursor) {
			.tree = &map->forest->trees[root],
			.height = BLOCK_MAP_TREE_HEIGHT - 1,
			.parent = cursors,
			.boundary = compute_boundary(map, root),
		};

		cursor->waiter.callback = launch_cursor;
		acquire_vio_from_pool(cursors->pool, &cursor->waiter);
	};
}
