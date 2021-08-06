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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/base/intMap.h#1 $
 */

#ifndef INT_MAP_H
#define INT_MAP_H

#include "common.h"

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

/**
 * Allocate and initialize an int_map.
 *
 * @param [in]  initial_capacity  the number of entries the map should
 *                                initially be capable of holding (zero tells
 *                                the map to use its own small default)
 * @param [in]  initial_load      the load factor of the map, expressed as an
 *                                integer percentage (typically in the range
 *                                50 to 90, with zero telling the map to use
 *                                its own default)
 * @param [out] map_ptr           a pointer to hold the new int_map
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_int_map(size_t initial_capacity,
			      unsigned int initial_load,
			      struct int_map **map_ptr);

/**
 * Free an int_map. NOTE: The map does not own the pointer values stored in the
 * map and they are not freed by this call.
 *
 * @param map  The int_map to free
 **/
void free_int_map(struct int_map *map);

/**
 * Get the number of entries stored in an int_map.
 *
 * @param map  the int_map to query
 *
 * @return the number of entries in the map
 **/
size_t int_map_size(const struct int_map *map);

/**
 * Retrieve the value associated with a given key from the int_map.
 *
 * @param map  the int_map to query
 * @param key  the key to look up
 *
 * @return the value associated with the given key, or <code>NULL</code>
 *         if the key is not mapped to any value
 **/
void *int_map_get(struct int_map *map, uint64_t key);

/**
 * Try to associate a value (a pointer) with an integer in an int_map. If the
 * map already contains a mapping for the provided key, the old value is
 * only replaced with the specified value if update is true. In either case
 * the old value is returned. If the map does not already contain a value for
 * the specified key, the new value is added regardless of the value of update.
 *
 * @param [in]  map           the int_map to attempt to modify
 * @param [in]  key           the key with which to associate the new value
 * @param [in]  new_value     the value to be associated with the key
 * @param [in]  update        whether to overwrite an existing value
 * @param [out] old_value_ptr  a pointer in which to store either the old value
 *                            (if the key was already mapped) or
 *                            <code>NULL</code> if the map did not contain the
 *                            key; <code>NULL</code> may be provided if the
 *                            caller does not need to know the old value
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check int_map_put(struct int_map *map,
			     uint64_t key,
			     void *new_value,
			     bool update,
			     void **old_value_ptr);

/**
 * Remove the mapping for a given key from the int_map.
 *
 * @param map  the int_map from which to remove the mapping
 * @param key  the key whose mapping is to be removed
 *
 * @return the value that was associated with the key, or
 *         <code>NULL</code> if it was not mapped
 **/
void *int_map_remove(struct int_map *map, uint64_t key);

#endif /* INT_MAP_H */
