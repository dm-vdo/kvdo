/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef FOREST_H
#define FOREST_H

#include "block-map-tree.h"
#include "types.h"

/**
 * typedef vdo_entry_callback - A function to be called for each allocated PBN
 *                              when traversing the forest.
 * @pbn: A PBN of a tree node.
 * @completion: The parent completion of the traversal.
 *
 * Return: VDO_SUCCESS or an error.
 */
typedef int vdo_entry_callback(physical_block_number_t pbn,
			       struct vdo_completion *completion);

struct tree_page * __must_check
vdo_get_tree_page_by_index(struct forest *forest,
			   root_count_t root_index,
			   height_t height,
			   page_number_t page_index);

int __must_check vdo_make_forest(struct block_map *map, block_count_t entries);

void vdo_free_forest(struct forest *forest);

void vdo_abandon_forest(struct block_map *map);

void vdo_replace_forest(struct block_map *map);

void vdo_traverse_forest(struct block_map *map,
			 vdo_entry_callback *callback,
			 struct vdo_completion *parent);

#endif /* FOREST_H */
