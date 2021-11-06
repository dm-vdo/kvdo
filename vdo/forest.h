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
 */

#ifndef FOREST_H
#define FOREST_H

#include "block-map-tree.h"
#include "types.h"

/**
 * A function to be called for each allocated PBN when traversing the forest.
 *
 * @param pbn         A PBN of a tree node
 * @param completion  The parent completion of the traversal
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int vdo_entry_callback(physical_block_number_t pbn,
			       struct vdo_completion *completion);

struct tree_page * __must_check
get_vdo_tree_page_by_index(struct forest *forest,
			   root_count_t root_index,
			   height_t height,
			   page_number_t page_index);

int __must_check make_vdo_forest(struct block_map *map, block_count_t entries);

void free_vdo_forest(struct forest *forest);

void abandon_vdo_forest(struct block_map *map);

void replace_vdo_forest(struct block_map *map);

void traverse_vdo_forest(struct block_map *map,
			 vdo_entry_callback *callback,
			 struct vdo_completion *parent);

#endif /* FOREST_H */
