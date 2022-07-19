// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "forest.h"

#include <linux/bio.h>

#include "logger.h"
#include "memory-alloc.h"

#include "block-map.h"
#include "block-map-page.h"
#include "block-map-tree.h"
#include "constants.h"
#include "dirty-lists.h"
#include "forest.h"
#include "io-submitter.h"
#include "num-utils.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "types.h"
#include "vdo.h"
#include "vio.h"
#include "vio-pool.h"

enum {
	BLOCK_MAP_VIO_POOL_SIZE = 64,
};

struct block_map_tree_segment {
	struct tree_page *levels[VDO_BLOCK_MAP_TREE_HEIGHT];
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
	struct cursor_level levels[VDO_BLOCK_MAP_TREE_HEIGHT];
	struct vio_pool_entry *vio_pool_entry;
};

struct cursors {
	struct block_map *map;
	struct block_map_tree_zone *zone;
	struct vio_pool *pool;
	vdo_entry_callback *entry_callback;
	struct vdo_completion *parent;
	root_count_t active_roots;
	struct cursor cursors[];
};

/**
 * *vdo_get_tree_page_by_index() - Get the tree page for a given height and
 *                                 page index.
 * @forest: The forest which holds the page.
 * @root_index: The index of the tree that holds the page.
 * @height: The height of the desired page.
 * @page_index: The index of the desired page.
 *
 * Return: The requested page.
 */
struct tree_page *vdo_get_tree_page_by_index(struct forest *forest,
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

static int make_segment(struct forest *old_forest,
			block_count_t new_pages,
			struct boundary *new_boundary,
			struct forest *forest)
{
	size_t index = (old_forest == NULL) ? 0 : old_forest->segments;
	struct tree_page *page_ptr;
	page_count_t segment_sizes[VDO_BLOCK_MAP_TREE_HEIGHT];
	height_t height;
	root_count_t root;
	int result;

	forest->segments = index + 1;

	result = UDS_ALLOCATE(forest->segments, struct boundary,
			      "forest boundary array", &forest->boundaries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(forest->segments,
			       struct tree_page *,
			       "forest page pointers",
			       &forest->pages);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(new_pages,
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

	for (height = 0; height < VDO_BLOCK_MAP_TREE_HEIGHT; height++) {
		segment_sizes[height] = new_boundary->levels[height];
		if (index > 0) {
			segment_sizes[height] -=
				old_forest->boundaries[index - 1].levels[height];
		}
	}

	page_ptr = forest->pages[index];
	for (root = 0; root < forest->map->root_count; root++) {
		struct block_map_tree_segment *segment;
		struct block_map_tree *tree = &(forest->trees[root]);
		height_t height;

		int result = UDS_ALLOCATE(forest->segments,
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

		segment = &(tree->segments[index]);
		for (height = 0; height < VDO_BLOCK_MAP_TREE_HEIGHT; height++) {
			if (segment_sizes[height] == 0) {
				continue;
			}

			segment->levels[height] = page_ptr;
			if (height == (VDO_BLOCK_MAP_TREE_HEIGHT - 1)) {
				/* Record the root. */
				struct block_map_page *page =
					vdo_format_block_map_page(page_ptr->page_buffer,
							      forest->map->nonce,
							      VDO_INVALID_PBN,
							      true);
				page->entries[0] =
					vdo_pack_pbn(forest->map->root_origin + root,
						     VDO_MAPPING_STATE_UNCOMPRESSED);
			}
			page_ptr += segment_sizes[height];
		}
	}

	return VDO_SUCCESS;
}

static void deforest(struct forest *forest, size_t first_page_segment)
{
	root_count_t root;

	if (forest->pages != NULL) {
		size_t segment;

		for (segment = first_page_segment; segment < forest->segments;
		     segment++) {
			UDS_FREE(forest->pages[segment]);
		}
		UDS_FREE(forest->pages);
	}

	for (root = 0; root < forest->map->root_count; root++) {
		struct block_map_tree *tree = &(forest->trees[root]);

		UDS_FREE(tree->segments);
	}

	UDS_FREE(forest->boundaries);
	UDS_FREE(forest);
}

/**
 * vdo_make_forest() - Make a collection of trees for a block_map, expanding
 *                     the existing forest if there is one.
 * @map: The block map.
 * @entries: The number of entries the block map will hold.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_make_forest(struct block_map *map, block_count_t entries)
{
	struct forest *forest, *old_forest = map->forest;
	struct boundary new_boundary, *old_boundary = NULL;
	block_count_t new_pages;
	int result;

	if (old_forest != NULL) {
		old_boundary =
			&(old_forest->boundaries[old_forest->segments - 1]);
	}

	new_pages = vdo_compute_new_forest_pages(map->root_count, old_boundary,
						 entries, &new_boundary);
	if (new_pages == 0) {
		map->next_entry_count = entries;
		return VDO_SUCCESS;
	}

	result = UDS_ALLOCATE_EXTENDED(struct forest, map->root_count,
				       struct block_map_tree, __func__,
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

/**
 * vdo_free_forest() - Free a forest and all of the segments it contains.
 * @forest: The forest to free.
 */
void vdo_free_forest(struct forest *forest)
{
	if (forest == NULL) {
		return;
	}

	deforest(forest, 0);
}

/**
 * vdo_abandon_forest() - Abandon the unused next forest from a block_map.
 * @map: The block map.
 */
void vdo_abandon_forest(struct block_map *map)
{
	struct forest *forest = map->next_forest;

	map->next_forest = NULL;
	if (forest != NULL) {
		deforest(forest, forest->segments - 1);
	}

	map->next_entry_count = 0;
}

/**
 * vdo_replace_forest() - Replace a block_map's forest with the
 *                        already-prepared larger forest.
 * @map: The block map.
 */
void vdo_replace_forest(struct block_map *map)
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
 * finish_cursor() - Finish the traversal of a single tree. If it was the last
 *                   cursor, finish the traversal.
 * @cursor: The cursor doing the traversal.
 */
static void finish_cursor(struct cursor *cursor)
{
	struct cursors *cursors = cursor->parent;
	struct vdo_completion *parent = cursors->parent;

	return_vio_to_pool(cursors->pool, cursor->vio_pool_entry);
	if (--cursors->active_roots > 0) {
		return;
	}

	UDS_FREE(cursors);

	vdo_finish_completion(parent, VDO_SUCCESS);
}

static void traverse(struct cursor *cursor);

/**
 * continue_traversal() - Continue traversing a block map tree.
 * @completion: The VIO doing a read or write.
 */
static void continue_traversal(struct vdo_completion *completion)
{
	struct vio_pool_entry *pool_entry = completion->parent;
	struct cursor *cursor = pool_entry->parent;

	record_metadata_io_error(as_vio(completion));
	traverse(cursor);
}

/**
 * finish_traversal_load() - Continue traversing a block map tree now that a
 *                           page has been loaded.
 * @completion: The VIO doing the read.
 */
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
	vdo_copy_valid_page(entry->buffer,
			    cursor->parent->map->nonce,
			    entry->vio->physical,
			    page);
	traverse(cursor);
}

static void traversal_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vio_pool_entry *entry = vio->completion.parent;
	struct cursor *cursor = entry->parent;

	continue_vio_after_io(vio,
			      finish_traversal_load,
			      cursor->parent->zone->map_zone->thread_id);
}

/**
 * traverse() - Traverse a single block map tree.
 * @cursor: The cursor doing the traversal.
 *
 * This is the recursive heart of the traversal process.
 */
static void traverse(struct cursor *cursor)
{
	for (; cursor->height < VDO_BLOCK_MAP_TREE_HEIGHT; cursor->height++) {
		height_t height = cursor->height;
		struct cursor_level *level = &cursor->levels[height];
		struct tree_page *tree_page =
			&(cursor->tree->segments[0]
				  .levels[height][level->page_index]);
		struct block_map_page *page =
			(struct block_map_page *) tree_page->page_buffer;
		if (!vdo_is_block_map_page_initialized(page)) {
			continue;
		}

		for (; level->slot < VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
		     level->slot++) {
			struct cursor_level *next_level;
			page_number_t entry_index =
				(VDO_BLOCK_MAP_ENTRIES_PER_PAGE *
				 level->page_index) + level->slot;

			struct data_location location =
				vdo_unpack_block_map_entry(&page->entries[level->slot]);
			if (!vdo_is_valid_location(&location)) {
				/*
				 * This entry is invalid, so remove it from the
				 * page.
				 */
				page->entries[level->slot] =
					vdo_pack_pbn(VDO_ZERO_BLOCK,
						     VDO_MAPPING_STATE_UNMAPPED);
				vdo_write_tree_page(tree_page,
						    cursor->parent->zone);
				continue;
			}

			if (!vdo_is_mapped_location(&location)) {
				continue;
			}

			/*
			 * Erase mapped entries past the end of the logical
			 * space.
			 */
			if (entry_index >= cursor->boundary.levels[height]) {
				page->entries[level->slot] =
					vdo_pack_pbn(VDO_ZERO_BLOCK,
						     VDO_MAPPING_STATE_UNMAPPED);
				vdo_write_tree_page(tree_page,
						    cursor->parent->zone);
				continue;
			}

			if (cursor->height < VDO_BLOCK_MAP_TREE_HEIGHT - 1) {
				int result =
					cursor->parent->entry_callback(location.pbn,
								       cursor->parent->parent);
				if (result != VDO_SUCCESS) {
					page->entries[level->slot] =
						vdo_pack_pbn(VDO_ZERO_BLOCK,
							     VDO_MAPPING_STATE_UNMAPPED);
					vdo_write_tree_page(tree_page,
							    cursor->parent->zone);
					continue;
				}
			}

			if (cursor->height == 0) {
				continue;
			}

			cursor->height--;
			next_level = &cursor->levels[cursor->height];
			next_level->page_index = entry_index;
			next_level->slot = 0;
			level->slot++;
			submit_metadata_vio(cursor->vio_pool_entry->vio,
					    location.pbn,
					    traversal_endio,
					    continue_traversal,
					    REQ_OP_READ | REQ_PRIO);
			return;
		}
	}

	finish_cursor(cursor);
}

/**
 * launch_cursor() - Start traversing a single block map tree now that the
 *                   cursor has a VIO with which to load pages.
 * @waiter: The cursor.
 * @context: The vio_pool_entry just acquired.
 *
 * Implements waiter_callback.
 */
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
 * compute_boundary() - Compute the number of pages used at each level of the
 *                      given root's tree.
 * @map: The block map.
 * @root_index: The index of the root to measure.
 *
 * Return: The list of page counts as a boundary structure.
 */
static struct boundary compute_boundary(struct block_map *map,
				       root_count_t root_index)
{
	struct boundary boundary;
	height_t height;

	page_count_t leaf_pages =
		vdo_compute_block_map_page_count(map->entry_count);

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

	for (height = 0; height < VDO_BLOCK_MAP_TREE_HEIGHT - 1; height++) {
		boundary.levels[height] = level_pages;
		level_pages = DIV_ROUND_UP(level_pages,
					   VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
	}

	/* The root node always exists, even if the root is otherwise unused. */
	boundary.levels[VDO_BLOCK_MAP_TREE_HEIGHT - 1] = 1;

	return boundary;
}

/**
 * vdo_traverse_forest() - Walk the entire forest of a block map.
 * @map: The block map to traverse.
 * @callback: A function to call with the pbn of each allocated node in
 *            the forest.
 * @parent: The completion to notify on each traversed PBN, and when
 *          the traversal is complete.
 */
void vdo_traverse_forest(struct block_map *map,
			 vdo_entry_callback *callback,
			 struct vdo_completion *parent)
{
	root_count_t root;
	struct cursors *cursors;
	int result = UDS_ALLOCATE_EXTENDED(struct cursors,
					   map->root_count,
					   struct cursor,
					   __func__,
					   &cursors);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	cursors->map = map;
	cursors->zone = &map->zones[0].tree_zone;
	cursors->pool = cursors->zone->vio_pool;
	cursors->entry_callback = callback;
	cursors->parent = parent;
	cursors->active_roots = map->root_count;
	for (root = 0; root < map->root_count; root++) {
		struct cursor *cursor = &cursors->cursors[root];
		*cursor = (struct cursor) {
			.tree = &map->forest->trees[root],
			.height = VDO_BLOCK_MAP_TREE_HEIGHT - 1,
			.parent = cursors,
			.boundary = compute_boundary(map, root),
		};

		cursor->waiter.callback = launch_cursor;
		acquire_vio_from_pool(cursors->pool, &cursor->waiter);
	};
}
