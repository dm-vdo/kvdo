/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INT_MAP_H
#define INT_MAP_H

#include "compiler.h"
#include "type-defs.h"

/**
 * DOC: int_map
 *
 * An int_map associates pointers (void *) with integer keys (uint64_t). NULL
 * pointer values are not supported.
 *
 * The map is implemented as hash table, which should provide constant-time
 * insert, query, and remove operations, although the insert may occasionally
 * grow the table, which is linear in the number of entries in the map. The
 * table will grow as needed to hold new entries, but will not shrink as
 * entries are removed.
 */

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
