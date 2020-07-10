/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/krusty/src/uds/searchList.c#6 $
 */

#include "searchList.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"

/**********************************************************************/
int make_search_list(unsigned int capacity, struct search_list **list_ptr)
{
	if (capacity == 0) {
		return logErrorWithStringError(UDS_INVALID_ARGUMENT,
					       "search list must have entries");
	}
	if (capacity > UINT8_MAX) {
		return logErrorWithStringError(UDS_INVALID_ARGUMENT,
					       "search list capacity must fit in 8 bits");
	}

	// We need three temporary entry arrays for purge_search_list().
	// Allocate them contiguously with the main array.
	unsigned int bytes = (sizeof(struct search_list) +
			      (4 * capacity * sizeof(uint8_t)));
	struct search_list *list;
	int result = allocate_cache_aligned(bytes, "search list", &list);
	if (result != UDS_SUCCESS) {
		return result;
	}

	list->capacity = capacity;
	list->first_dead_entry = 0;

	// Fill in the indexes of the chapter index cache entries. These will
	// be only ever be permuted as the search list is used.
	uint8_t i;
	for (i = 0; i < capacity; i++) {
		list->entries[i] = i;
	}

	*list_ptr = list;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_search_list(struct search_list **list_ptr)
{
	FREE(*list_ptr);
	*list_ptr = NULL;
}

/**********************************************************************/
void purge_search_list(struct search_list *search_list,
		       const struct cached_chapter_index chapters[],
		       uint64_t oldest_virtual_chapter)
{
	if (search_list->first_dead_entry == 0) {
		// There are no live entries in the list to purge.
		return;
	}

	/*
	 * Partition the previously-alive entries in the list into three
	 * temporary lists, keeping the current LRU search order within each
	 * list. The element array was allocated with enough space for all four
	 * lists.
	 */
	uint8_t *entries = &search_list->entries[0];
	uint8_t *alive = &entries[search_list->capacity];
	uint8_t *skipped = &alive[search_list->capacity];
	uint8_t *dead = &skipped[search_list->capacity];
	unsigned int next_alive = 0;
	unsigned int next_skipped = 0;
	unsigned int next_dead = 0;

	int i;
	for (i = 0; i < search_list->first_dead_entry; i++) {
		uint8_t entry = entries[i];
		const struct cached_chapter_index *chapter = &chapters[entry];
		if ((chapter->virtual_chapter < oldest_virtual_chapter) ||
		    (chapter->virtual_chapter == UINT64_MAX)) {
			dead[next_dead++] = entry;
		} else if (chapter->skip_search) {
			skipped[next_skipped++] = entry;
		} else {
			alive[next_alive++] = entry;
		}
	}

	// Copy the temporary lists back to the search list so we wind up with
	// [ alive, alive, skippable, new-dead, new-dead, old-dead, old-dead ]
	memcpy(entries, alive, next_alive);
	entries += next_alive;

	memcpy(entries, skipped, next_skipped);
	entries += next_skipped;

	memcpy(entries, dead, next_dead);
	// The first dead entry is now the start of the copied dead list.
	search_list->first_dead_entry = (next_alive + next_skipped);
}
