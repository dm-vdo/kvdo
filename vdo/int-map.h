/* SPDX-License-Identifier: GPL-2.0-only */
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

#ifndef INT_MAP_H
#define INT_MAP_H

#include "compiler.h"
#include "type-defs.h"

/**
 * An int_map associates pointers (<code>void *</code>) with integer keys
 * (<code>uint64_t</code>). <code>NULL</code> pointer values are not
 * supported.
 *
 * The map is implemented as hash table, which should provide constant-time
 * insert, query, and remove operations, although the insert may occasionally
 * grow the table, which is linear in the number of entries in the map. The
 * table will grow as needed to hold new entries, but will not shrink as
 * entries are removed.
 **/

struct int_map;

int __must_check make_int_map(size_t initial_capacity,
			      unsigned int initial_load,
			      struct int_map **map_ptr);

void free_int_map(struct int_map *map);

size_t int_map_size(const struct int_map *map);

void *int_map_get(struct int_map *map, uint64_t key);

int __must_check int_map_put(struct int_map *map,
			     uint64_t key,
			     void *new_value,
			     bool update,
			     void **old_value_ptr);

void *int_map_remove(struct int_map *map, uint64_t key);

#endif /* INT_MAP_H */
