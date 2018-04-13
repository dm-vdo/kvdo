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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/intMap.c#1 $
 */

/**
 * Hash table implementation of a map from integers to pointers, implemented
 * using the Hopscotch Hashing algorithm by Herlihy, Shavit, and Tzafrir (see
 * http://en.wikipedia.org/wiki/Hopscotch_hashing). This implementation does
 * not contain any of the locking/concurrency features of the algorithm, just
 * the collision resolution scheme.
 *
 * Hopscotch Hashing is based on hashing with open addressing and linear
 * probing. All the entries are stored in a fixed array of buckets, with no
 * dynamic allocation for collisions. Unlike linear probing, all the entries
 * that hash to a given bucket are stored within a fixed neighborhood starting
 * at that bucket. Chaining is effectively represented as a bit vector
 * relative to each bucket instead of as pointers or explicit offsets.
 *
 * When an empty bucket cannot be found within a given neighborhood,
 * subsequent neighborhoods are searched, and one or more entries will "hop"
 * into those neighborhoods. When this process works, an empty bucket will
 * move into the desired neighborhood, allowing the entry to be added. When
 * that process fails (typically when the buckets are around 90% full), the
 * table must be resized and the all entries rehashed and added to the
 * expanded table.
 *
 * Unlike linear probing, the number of buckets that must be searched in the
 * worst case has a fixed upper bound (the size of the neighborhood). Those
 * entries occupy a small number of memory cache lines, leading to improved
 * use of the cache (fewer misses on both successful and unsuccessful
 * searches). Hopscotch hashing outperforms linear probing at much higher load
 * factors, so even with the increased memory burden for maintaining the hop
 * vectors, less memory is needed to achieve that performance. Hopscotch is
 * also immune to "contamination" from deleting entries since entries are
 * genuinely removed instead of being replaced by a placeholder.
 *
 * The published description of the algorithm used a bit vector, but the paper
 * alludes to an offset scheme which is used by this implementation. Since the
 * entries in the neighborhood are within N entries of the hash bucket at the
 * start of the neighborhood, a pair of small offset fields each log2(N) bits
 * wide is all that's needed to maintain the hops as a linked list. In order
 * to encode "no next hop" (i.e. NULL) as the natural initial value of zero,
 * the offsets are biased by one (i.e. 0 => NULL, 1 => offset=0, 2 =>
 * offset=1, etc.) We can represent neighborhoods of up to 255 entries with
 * just 8+8=16 bits per entry. The hop list is sorted by hop offset so the
 * first entry in the list is always the bucket closest to the start of the
 * neighborhood.
 *
 * While individual accesses tend to be very fast, the table resize operations
 * are very very expensive. If an upper bound on the latency of adding an
 * entry to the table is needed, we either need to ensure the table is
 * pre-sized to be large enough so no resize is ever needed, or we'll need to
 * develop an approach to incrementally resize the table.
 **/

#include "intMap.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

enum {
  DEFAULT_CAPACITY = 16,    // the number of neighborhoods in a new table
  NEIGHBORHOOD     = 255,   // the number of buckets in each neighborhood
  MAX_PROBES       = 1024,  // limit on the number of probes for a free bucket
  NULL_HOP_OFFSET  = 0,     // the hop offset value terminating the hop list
  DEFAULT_LOAD     = 75     // a compromise between memory use and performance
};

/**
 * Buckets are packed together to reduce memory usage and improve cache
 * efficiency. It would be tempting to encode the hop offsets separately and
 * maintain alignment of key/value pairs, but it's crucial to keep the hop
 * fields near the buckets that they use them so they'll tend to share cache
 * lines.
 **/
typedef struct __attribute__((packed)) bucket {
  uint8_t   firstHop;  // the biased offset of the first entry in the hop list
                       // of the neighborhood that hashes to this bucket
  uint8_t   nextHop;   // the biased offset of the next bucket in the hop list

  uint64_t  key;       // the key stored in this bucket
  void     *value;     // the value stored in this bucket (NULL if empty)
} Bucket;

/**
 * The concrete definition of the opaque IntMap type. To avoid having to wrap
 * the neighborhoods of the last entries back around to the start of the
 * bucket array, we allocate a few more buckets at the end of the array
 * instead, which is why capacity and bucketCount are different.
 **/
struct intMap {
  size_t  size;         // the number of entries stored in the map
  size_t  capacity;     // the number of neighborhoods in the map
  size_t  bucketCount;  // the number of buckets in the bucket array
  Bucket *buckets;      // the array of hash buckets
};

/**
 * This is the Google CityHash 16-byte hash mixing function.
 *
 * @param input1  the first input value
 * @param input2  the second input value
 *
 * @return a hash of the two inputs
 **/
static uint64_t mix(uint64_t input1, uint64_t input2)
{
  static const uint64_t CITY_MULTIPLIER = 0x9ddfea08eb382d69ULL;

  uint64_t hash = (input1 ^ input2);
  hash *= CITY_MULTIPLIER;
  hash ^= (hash >> 47);
  hash ^= input2;
  hash *= CITY_MULTIPLIER;
  hash ^= (hash >> 47);
  hash *= CITY_MULTIPLIER;
  return hash;
}

/**
 * Calculate a 64-bit non-cryptographic hash value for the provided 64-bit
 * integer key. The implementation is based on Google's CityHash, only
 * handling the specific case of an 8-byte input.
 *
 * @param key  the mapping key
 *
 * @return the hash of the mapping key
 **/
static uint64_t hashKey(uint64_t key)
{
  // Aliasing restrictions forbid us from casting pointer types, so use a
  // union to convert a single uint64_t to two uint32_t values.
  union {
    uint64_t u64;
    uint32_t u32[2];
  } pun = { .u64 = key };
  return mix(sizeof(key) + (((uint64_t) pun.u32[0]) << 3), pun.u32[1]);
}

/**
 * Initialize an IntMap.
 *
 * @param map       the map to initialize
 * @param capacity  the initial capacity of the map
 *
 * @return UDS_SUCCESS or an error code
 **/
static int allocateBuckets(IntMap *map, size_t capacity)
{
  map->size     = 0;
  map->capacity = capacity;

  // Allocate NEIGHBORHOOD - 1 extra buckets so the last bucket can have a
  // full neighborhood without have to wrap back around to element zero.
  map->bucketCount = capacity + (NEIGHBORHOOD - 1);
  return ALLOCATE(map->bucketCount, Bucket, "IntMap buckets", &map->buckets);
}

/**********************************************************************/
int makeIntMap(size_t         initialCapacity,
               unsigned int   initialLoad,
               IntMap       **mapPtr)
{
  // Use the default initial load if the caller did not specify one.
  if (initialLoad == 0) {
    initialLoad = DEFAULT_LOAD;
  }
  if (initialLoad > 100) {
    return UDS_INVALID_ARGUMENT;
  }

  IntMap *map;
  int result = ALLOCATE(1, IntMap, "IntMap", &map);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Use the default capacity if the caller did not specify one.
  size_t capacity = (initialCapacity > 0) ? initialCapacity : DEFAULT_CAPACITY;

  // Scale up the capacity by the specified initial load factor.
  // (i.e to hold 1000 entries at 80% load we need a capacity of 1250)
  capacity = capacity * 100 / initialLoad;

  result = allocateBuckets(map, capacity);
  if (result != UDS_SUCCESS) {
    freeIntMap(&map);
    return result;
  }

  *mapPtr = map;
  return UDS_SUCCESS;
}

/**
 * Free the bucket array for the map.
 *
 * @param map  the map whose bucket array is to be freed
 **/
static void freeBuckets(IntMap *map)
{
  FREE(map->buckets);
  map->buckets = NULL;
}

/**********************************************************************/
void freeIntMap(IntMap **mapPtr)
{
  if (*mapPtr != NULL) {
    freeBuckets(*mapPtr);
    FREE(*mapPtr);
    *mapPtr = NULL;
  }
}

/**********************************************************************/
size_t intMapSize(const IntMap *map)
{
  return map->size;
}

/**
 * Convert a biased hop offset within a neighborhood to a pointer to the
 * bucket it references.
 *
 * @param neighborhood  the first bucket in the neighborhood
 * @param hopOffset     the biased hop offset to the desired bucket
 *
 * @return <code>NULL</code> if hopOffset is zero, otherwise a pointer to
 *         the bucket in the neighborhood at <code>hopOffset - 1</code>
 **/
static Bucket *dereferenceHop(Bucket *neighborhood, unsigned int hopOffset)
{
  if (hopOffset == NULL_HOP_OFFSET) {
    return NULL;
  }

  STATIC_ASSERT(NULL_HOP_OFFSET == 0);
  return &neighborhood[hopOffset - 1];
}

/**
 * Add a bucket into the hop list for the neighborhood, inserting it into the
 * list so the hop list remains sorted by hop offset.
 *
 * @param neighborhood  the first bucket in the neighborhood
 * @param newBucket     the bucket to add to the hop list
 **/
static void insertInHopList(Bucket *neighborhood, Bucket *newBucket)
{
  // Zero indicates a NULL hop offset, so bias the hop offset by one.
  int hopOffset = 1 + (newBucket - neighborhood);

  // Handle the special case of adding a bucket at the start of the list.
  int nextHop = neighborhood->firstHop;
  if ((nextHop == NULL_HOP_OFFSET) || (nextHop > hopOffset)) {
    newBucket->nextHop = nextHop;
    neighborhood->firstHop = hopOffset;
    return;
  }

  // Search the hop list for the insertion point that maintains the sort
  // order.
  for (;;) {
    Bucket *bucket = dereferenceHop(neighborhood, nextHop);
    nextHop = bucket->nextHop;

    if ((nextHop == NULL_HOP_OFFSET) || (nextHop > hopOffset)) {
      newBucket->nextHop = nextHop;
      bucket->nextHop = hopOffset;
      return;
    }
  }
}

/**
 * Select and return the hash bucket for a given search key.
 *
 * @param map  the map to search
 * @param key  the mapping key
 **/
static Bucket *selectBucket(const IntMap *map, uint64_t key)
{
  // Calculate a good hash value for the provided key. We want exactly 32
  // bits, so mask the result.
  uint64_t hash = hashKey(key) & 0xFFFFFFFF;

  /*
   * Scale the 32-bit hash to a bucket index by treating it as a binary
   * fraction and multiplying that by the capacity. If the hash is uniformly
   * distributed over [0 .. 2^32-1], then (hash * capacity / 2^32) should be
   * uniformly distributed over [0 .. capacity-1]. The multiply and shift is
   * much faster than a divide (modulus) on X86 CPUs.
   */
  return &map->buckets[(hash * map->capacity) >> 32];
}

/**
 * Search the hop list associated with given hash bucket for a given search
 * key. If the key is found, returns a pointer to the entry (bucket or
 * collision), otherwise returns <code>NULL</code>.
 *
 * @param [in]  map          the map being searched
 * @param [in]  bucket       the map bucket to search for the key
 * @param [in]  key          the mapping key
 * @param [out] previousPtr  if not <code>NULL</code>, a pointer in which to
 *                           store the bucket in the list preceding the one
 *                           that had the matching key
 *
 * @return an entry that matches the key, or <code>NULL</code> if not found
 **/
static Bucket *searchHopList(IntMap    *map __attribute__((unused)),
                             Bucket    *bucket,
                             uint64_t   key,
                             Bucket   **previousPtr)
{
  Bucket *previous = NULL;
  unsigned int nextHop = bucket->firstHop;
  while (nextHop != NULL_HOP_OFFSET) {
    // Check the neighboring bucket indexed by the offset for the desired key.
    Bucket *entry = dereferenceHop(bucket, nextHop);
    if ((key == entry->key) && (entry->value != NULL)) {
      if (previousPtr != NULL) {
        *previousPtr = previous;
      }
      return entry;
    }
    nextHop = entry->nextHop;
    previous = entry;
  }
  return NULL;
}

/**********************************************************************/
void *intMapGet(IntMap *map, uint64_t key)
{
  Bucket *match = searchHopList(map, selectBucket(map, key), key, NULL);
  return ((match != NULL) ? match->value : NULL);
}

/**
 * Increase the number of hash buckets and rehash all the existing entries,
 * storing them in the new buckets.
 *
 * @param map  the map to resize
 **/
static int resizeBuckets(IntMap *map)
{
  // Copy the top-level map data to the stack.
  IntMap oldMap = *map;

  // Re-initialize the map to be empty and 50% larger.
  size_t newCapacity = map->capacity / 2 * 3;
  logInfo("%s: attempting resize from %zu to %zu, current size=%zu",
          __func__, map->capacity, newCapacity, map->size);
  int result = allocateBuckets(map, newCapacity);
  if (result != UDS_SUCCESS) {
    *map = oldMap;
    return result;
  }

  // Populate the new hash table from the entries in the old bucket array.
  for (size_t i = 0; i < oldMap.bucketCount; i++) {
    Bucket *entry = &oldMap.buckets[i];
    if (entry->value == NULL) {
      continue;
    }

    result = intMapPut(map, entry->key, entry->value, true, NULL);
    if (result != UDS_SUCCESS) {
      // Destroy the new partial map and restore the map from the stack.
      freeBuckets(map);
      *map = oldMap;
      return result;
    }
  }

  // Destroy the old bucket array.
  freeBuckets(&oldMap);
  return UDS_SUCCESS;
}

/**
 * Probe the bucket array starting at the given bucket for the next empty
 * bucket, returning a pointer to it. <code>NULL</code> will be returned if
 * the search reaches the end of the bucket array or if the number of linear
 * probes exceeds a specified limit.
 *
 * @param map        the map containing the buckets to search
 * @param bucket     the bucket at which to start probing
 * @param maxProbes  the maximum number of buckets to search
 *
 * @return the next empty bucket, or <code>NULL</code> if the search failed
 **/
static Bucket *findEmptyBucket(IntMap       *map,
                               Bucket       *bucket,
                               unsigned int  maxProbes)
{
  // Limit the search to either the nearer of the end of the bucket array or a
  // fixed distance beyond the initial bucket.
  size_t remaining = &map->buckets[map->bucketCount] - bucket;
  Bucket *sentinel = &bucket[minSizeT(remaining, maxProbes)];

  for (Bucket *entry = bucket; entry < sentinel; entry++) {
    if (entry->value == NULL) {
      return entry;
    }
  }
  return NULL;
}

/**
 * Move an empty bucket closer to the start of the bucket array. This searches
 * the neighborhoods that contain the empty bucket for a non-empty bucket
 * closer to the start of the array. If such a bucket is found, this swaps the
 * two buckets by moving the entry to the empty bucket.
 *
 * @param map   the map containing the bucket
 * @param hole  the empty bucket to fill with an entry that precedes it in one
 *              of its enclosing neighborhoods
 *
 * @return the bucket that was vacated by moving its entry to the provided
 *         hole, or <code>NULL</code> if no entry could be moved
 **/
static Bucket *moveEmptyBucket(IntMap *map __attribute__((unused)),
                               Bucket *hole)
{
  /*
   * Examine every neighborhood that the empty bucket is part of, starting
   * with the one in which it is the last bucket. No boundary check is needed
   * for the negative array arithmetic since this function is only called when
   * hole is at least NEIGHBORHOOD cells deeper into the array than a valid
   * bucket.
   */
  for (Bucket *bucket = &hole[1 - NEIGHBORHOOD]; bucket < hole; bucket++) {
    // Find the entry that is nearest to the bucket, which means it will be
    // nearest to the hash bucket whose neighborhood is full.
    Bucket *newHole = dereferenceHop(bucket, bucket->firstHop);
    if (newHole == NULL) {
      // There are no buckets in this neighborhood that are in use by this one
      // (they must all be owned by overlapping neighborhoods).
      continue;
    }

    // Skip this bucket if its first entry is actually further away than the
    // hole that we're already trying to fill.
    if (hole < newHole) {
      continue;
    }

    /*
     * We've found an entry in this neighborhood that we can "hop" further
     * away, moving the hole closer to the hash bucket, if not all the way
     * into its neighborhood.
     */

    // The entry that will be the new hole is the first bucket in the list,
    // so setting firstHop is all that's needed remove it from the list.
    bucket->firstHop = newHole->nextHop;
    newHole->nextHop = NULL_HOP_OFFSET;

    // Move the entry into the original hole.
    hole->key      = newHole->key;
    hole->value    = newHole->value;
    newHole->value = NULL;

    // Insert the filled hole into the hop list for the neighborhood.
    insertInHopList(bucket, hole);
    return newHole;
  }

  // We couldn't find an entry to relocate to the hole.
  return NULL;
}

/**
 * Find and update any existing mapping for a given key, returning the value
 * associated with the key in the provided pointer.
 *
 * @param [in]  map           the IntMap to attempt to modify
 * @param [in]  neighborhood  the first bucket in the neighborhood that
 *                            would contain the search key
 * @param [in]  key           the key with which to associate the new value
 * @param [in]  newValue      the value to be associated with the key
 * @param [in]  update        whether to overwrite an existing value
 * @param [out] oldValuePtr   a pointer in which to store the old value
 *                            (unmodified if no mapping was found)
 *
 * @return <code>true</code> if the map contains a mapping for the key
 *         <code>false</code> if it does not
 **/
static bool updateMapping(IntMap    *map,
                          Bucket    *neighborhood,
                          uint64_t   key,
                          void      *newValue,
                          bool       update,
                          void     **oldValuePtr)
{
  Bucket *bucket = searchHopList(map, neighborhood, key, NULL);
  if (bucket == NULL) {
    // There is no bucket containing the key in the neighborhood.
    return false;
  }

  // Return the value of the current mapping (if desired) and update the
  // mapping with the new value (if desired).
  if (oldValuePtr != NULL) {
    *oldValuePtr = bucket->value;
  }
  if (update) {
    bucket->value = newValue;
  }
  return true;
}

/**
 * Find an empty bucket in a specified neighborhood for a new mapping or
 * attempt to re-arrange mappings so there is such a bucket. This operation
 * may fail (returning NULL) if an empty bucket is not available or could not
 * be relocated to the neighborhood.
 *
 * @param map           the IntMap to search or modify
 * @param neighborhood  the first bucket in the neighborhood in which
 *                      an empty bucket is needed for a new mapping
 *
 * @return a pointer to an empty bucket in the desired neighborhood, or
 *         <code>NULL</code> if a vacancy could not be found or arranged
 **/
static Bucket *findOrMakeVacancy(IntMap *map, Bucket *neighborhood)
{
  // Probe within and beyond the neighborhood for the first empty bucket.
  Bucket *hole = findEmptyBucket(map, neighborhood, MAX_PROBES);

  // Keep trying until the empty bucket is in the bucket's neighborhood or we
  // are unable to move it any closer by swapping it with a filled bucket.
  while (hole != NULL) {
    int distance = hole - neighborhood;
    if (distance < NEIGHBORHOOD) {
      // We've found or relocated an empty bucket close enough to the initial
      // hash bucket to be referenced by its hop vector.
      return hole;
    }

    // The nearest empty bucket isn't within the neighborhood that must
    // contain the new entry, so try to swap it with bucket that is closer.
    hole = moveEmptyBucket(map, hole);
  }

  return NULL;
}

/**********************************************************************/
int intMapPut(IntMap    *map,
              uint64_t   key,
              void      *newValue,
              bool       update,
              void     **oldValuePtr)
{
  if (newValue == NULL) {
    return UDS_INVALID_ARGUMENT;
  }

  // Select the bucket at the start of the neighborhood that must contain any
  // entry for the provided key.
  Bucket *neighborhood = selectBucket(map, key);

  // Check whether the neighborhood already contains an entry for the key, in
  // which case we optionally update it, returning the old value.
  if (updateMapping(map, neighborhood, key, newValue, update, oldValuePtr)) {
    return UDS_SUCCESS;
  }

  /*
   * Find an empty bucket in the desired neighborhood for the new entry or
   * re-arrange entries in the map so there is such a bucket. This operation
   * will usually succeed; the loop body will only be executed on the rare
   * occasions that we have to resize the map.
   */
  Bucket *bucket;
  while ((bucket = findOrMakeVacancy(map, neighborhood)) == NULL) {
    /*
     * There is no empty bucket in which to put the new entry in the current
     * map, so we're forced to allocate a new bucket array with a larger
     * capacity, re-hash all the entries into those buckets, and try again (a
     * very expensive operation for large maps).
     */
    int result = resizeBuckets(map);
    if (result != UDS_SUCCESS) {
      return result;
    }

    // Resizing the map invalidates all pointers to buckets, so recalculate
    // the neighborhood pointer.
    neighborhood = selectBucket(map, key);
  }

  // Put the new entry in the empty bucket, adding it to the neighborhood.
  bucket->key   = key;
  bucket->value = newValue;
  insertInHopList(neighborhood, bucket);
  map->size += 1;

  // There was no existing entry, so there was no old value to be returned.
  if (oldValuePtr != NULL) {
    *oldValuePtr = NULL;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
void *intMapRemove(IntMap *map, uint64_t key)
{
  // Select the bucket to search and search it for an existing entry.
  Bucket *bucket = selectBucket(map, key);
  Bucket *previous;
  Bucket *victim = searchHopList(map, bucket, key, &previous);

  if (victim == NULL) {
    // There is no matching entry to remove.
    return NULL;
  }

  // We found an entry to remove. Save the mapped value to return later and
  // empty the bucket.
  map->size -= 1;
  void *value   = victim->value;
  victim->value = NULL;
  victim->key   = 0;

  // The victim bucket is now empty, but it still needs to be spliced out of
  // the hop list.
  if (previous == NULL) {
    // The victim is the head of the list, so swing firstHop.
    bucket->firstHop  = victim->nextHop;
  } else {
    previous->nextHop = victim->nextHop;
  }
  victim->nextHop = NULL_HOP_OFFSET;

  return value;
}
