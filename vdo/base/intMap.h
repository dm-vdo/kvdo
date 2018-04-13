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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/intMap.h#1 $
 */

#ifndef INT_MAP_H
#define INT_MAP_H

#include "common.h"

/**
 * IntMap associates pointers (<code>void *</code>) with integer keys
 * (<code>uint64_t</code>). <code>NULL</code> pointer values are not
 * supported.
 *
 * The map is implemented as hash table, which should provide constant-time
 * insert, query, and remove operations, although the insert may occasionally
 * grow the table, which is linear in the number of entries in the map. The
 * table will grow as needed to hold new entries, but will not shrink as
 * entries are removed.
 **/

typedef struct intMap IntMap;

/**
 * Allocate and initialize an IntMap.
 *
 * @param [in]  initialCapacity  the number of entries the map should
 *                               initially be capable of holding (zero tells
 *                               the map to use its own small default)
 * @param [in]  initialLoad      the load factor of the map, expressed as an
 *                               integer percentage (typically in the range
 *                               50 to 90, with zero telling the map to use
 *                               its own default)
 * @param [out] mapPtr           a pointer to hold the new IntMap
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeIntMap(size_t         initialCapacity,
               unsigned int   initialLoad,
               IntMap       **mapPtr)
  __attribute__((warn_unused_result));

/**
 * Free an IntMap and null out the reference to it. NOTE: The map does not own
 * the pointer values stored in the map and they are not freed by this call.
 *
 * @param [in,out] mapPtr  the reference to the IntMap to free
 **/
void freeIntMap(IntMap **mapPtr);

/**
 * Get the number of entries stored in an IntMap.
 *
 * @param map  the IntMap to query
 *
 * @return the number of entries in the map
 **/
size_t intMapSize(const IntMap *map);

/**
 * Retrieve the value associated with a given key from the IntMap.
 *
 * @param map  the IntMap to query
 * @param key  the key to look up
 *
 * @return the value associated with the given key, or <code>NULL</code>
 *         if the key is not mapped to any value
 **/
void *intMapGet(IntMap *map, uint64_t key);

/**
 * Try to associate a value (a pointer) with an integer in an IntMap. If the
 * map already contains a mapping for the provided key, the old value is
 * only replaced with the specified value if update is true. In either case
 * the old value is returned. If the map does not already contain a value for
 * the specified key, the new value is added regardless of the value of update.
 *
 * @param [in]  map          the IntMap to attempt to modify
 * @param [in]  key          the key with which to associate the new value
 * @param [in]  newValue     the value to be associated with the key
 * @param [in]  update       whether to overwrite an existing value
 * @param [out] oldValuePtr  a pointer in which to store either the old value
 *                           (if the key was already mapped) or
 *                           <code>NULL</code> if the map did not contain the
 *                           key; <code>NULL</code> may be provided if the
 *                           caller does not need to know the old value
 *
 * @return UDS_SUCCESS or an error code
 **/
int intMapPut(IntMap    *map,
              uint64_t   key,
              void      *newValue,
              bool       update,
              void     **oldValuePtr)
  __attribute__((warn_unused_result));

/**
 * Remove the mapping for a given key from the IntMap.
 *
 * @param map  the IntMap from which to remove the mapping
 * @param key  the key whose mapping is to be removed
 *
 * @return the value that was associated with the key, or
 *         <code>NULL</code> if it was not mapped
 **/
void *intMapRemove(IntMap *map, uint64_t key);

#endif /* INT_MAP_H */
