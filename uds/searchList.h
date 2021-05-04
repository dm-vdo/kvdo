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
 * $Id: //eng/uds-releases/krusty/src/uds/searchList.h#5 $
 */

#ifndef SEARCH_LIST_H
#define SEARCH_LIST_H

#include "cachedChapterIndex.h"
#include "compiler.h"
#include "stringUtils.h"
#include "typeDefs.h"

/**
 * A search_list represents the permutations of the sparse chapter index cache
 * entry array. Those permutations express an ordering on the chapter indexes,
 * from most recently accessed to least recently accessed, which is the order
 * in which the indexes should be searched and the reverse order in which they
 * should be evicted from the cache (LRU cache replacement policy).
 *
 * Cache entries that are dead (virtual_chapter == UINT64_MAX) are kept as a
 * suffix of the list, avoiding the need to even iterate over them to search,
 * and ensuring that dead entries are replaced before any live entries are
 * evicted.
 *
 * The search list is intended to be instantated for each zone thread,
 * avoiding any need for synchronization. The structure is allocated on a
 * cache boundary to avoid false sharing of memory cache lines between zone
 * threads.
 **/
struct search_list {
	/** The number of cached chapter indexes and search list entries */
	uint8_t capacity;

	/** The index in the entries array of the first dead cache entry */
	uint8_t first_dead_entry;

	/** The chapter array indexes representing the chapter search order */
	uint8_t entries[];
};

/**
 * search_list_iterator captures the fields needed to iterate over the live
 * entries in a search list and return the struct cached_chapter_index pointers
 * that the search code actually wants to deal with.
 **/
struct search_list_iterator {
	/** The search list defining the chapter search iteration order */
	struct search_list *list;

	/** The index of the next entry to return from the search list */
	unsigned int next_entry;

	/** The cached chapters that are referenced by the search list */
	struct cached_chapter_index *chapters;
};

/**
 * Allocate and initialize a new chapter cache search list with the same
 * capacity as the cache. The index of each entry in the cache will appear
 * exactly once in the array. All the chapters in the cache are assumed to be
 * initially dead, so first_dead_entry will be zero and no chapters will be
 * returned when the search list is iterated.
 *
 * @param [in]  capacity  the number of entries in the search list
 * @param [out] list_ptr  a pointer in which to return the new search list
 **/
int __must_check make_search_list(unsigned int capacity,
				  struct search_list **list_ptr);

/**
 * Free a search list and null out the reference to it.
 *
 * @param list_ptr the reference to the search list to free
 **/
void free_search_list(struct search_list **list_ptr);

/**
 * Copy the contents of one search list to another.
 *
 * @param source  the list to copy
 * @param target  the list to replace
 **/
static INLINE void copy_search_list(const struct search_list *source,
				    struct search_list *target)
{
	*target = *source;
	memcpy(target->entries, source->entries, source->capacity);
}

/**
 * Prepare to iterate over the live cache entries a search list.
 *
 * @param list      the list defining the live chapters and the search order
 * @param chapters  the chapter index entries to return from get_next_chapter()
 *
 * @return an iterator positioned at the start of the search list
 **/
static INLINE struct search_list_iterator
iterate_search_list(struct search_list *list,
		    struct cached_chapter_index chapters[])
{
	struct search_list_iterator iterator = {
		.list = list,
		.next_entry = 0,
		.chapters = chapters,
	};
	return iterator;
}

/**
 * Check if the search list iterator has another entry to return.
 *
 * @param iterator  the search list iterator
 *
 * @return <code>true</code> if get_next_chapter() may be called
 **/
static INLINE bool
has_next_chapter(const struct search_list_iterator *iterator)
{
	return (iterator->next_entry < iterator->list->first_dead_entry);
}

/**
 * Return a pointer to the next live chapter in the search list iteration and
 * advance the iterator. This must only be called when has_next_chapter()
 * returns <code>true</code>.
 *
 * @param iterator  the search list iterator
 *
 * @return a pointer to the next live chapter index in the search list order
 **/
static INLINE struct cached_chapter_index *
get_next_chapter(struct search_list_iterator *iterator)
{
	return &iterator->chapters[iterator->list
					   ->entries[iterator->next_entry++]];
}

/**
 * Rotate the pointers in a prefix of a search list downwards by one item,
 * pushing elements deeper into the list and moving a new chapter to the start
 * of the search list. This is the "make most recent" operation on the search
 * list.
 *
 * If the search list provided is <code>[ 0 1 2 3 4 ]</code> and the prefix
 * length is <code>4</code>, then <code>3</code> is being moved to the front.
 * The search list after the call will be <code>[ 3 0 1 2 4 ]</code> and the
 * function will return <code>3</code>.
 *
 * @param search_list    the chapter index search list to rotate
 * @param prefix_length  the length of the prefix of the list to rotate
 *
 * @return the array index of the chapter cache entry that is now at the front
 *         of the search list
 **/
static INLINE uint8_t rotate_search_list(struct search_list *search_list,
					 uint8_t prefix_length)
{
	// Grab the value of the last entry in the list prefix.
	uint8_t most_recent = search_list->entries[prefix_length - 1];

	if (prefix_length > 1) {
		// Push the first N-1 entries down by one entry, overwriting
		// the entry we just grabbed.
		memmove(&search_list->entries[1],
			&search_list->entries[0],
			prefix_length - 1);

		// We now have a hole at the front of the list in which we can
		// place the rotated entry.
		search_list->entries[0] = most_recent;
	}

	// This function is also used to move a dead chapter to the front of
	// the list, in which case the suffix of dead chapters was pushed down
	// too.
	if (search_list->first_dead_entry < prefix_length) {
		search_list->first_dead_entry += 1;
	}

	return most_recent;
}

/**
 * Purge invalid cache entries, marking them as dead and moving them to the
 * end of the search list, then push any chapters that have skip_search set
 * down so they follow all the remaining live, valid chapters in the search
 * list. This effectively sorts the search list into three regions--active,
 * skippable, and dead--while maintaining the LRU ordering that already
 * existed (a stable sort).
 *
 * This operation must only be called during the critical section in
 * update_sparse_cache() since it effectively changes cache membership.
 *
 * @param search_list             the chapter index search list to purge
 * @param chapters                the chapter index cache entries
 * @param oldest_virtual_chapter  the oldest virtual chapter
 **/
void purge_search_list(struct search_list *search_list,
		       const struct cached_chapter_index chapters[],
		       uint64_t oldest_virtual_chapter);

#endif /* SEARCH_LIST_H */
