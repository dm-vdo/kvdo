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

#include "searchList.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"

/**********************************************************************/
int make_search_list(unsigned int capacity, struct search_list **list_ptr)
{
	struct search_list *list;
	unsigned int bytes;
	uint8_t i;
	int result;
	if (capacity == 0) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "search list must have entries");
	}
	if (capacity > UINT8_MAX) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "search list capacity must fit in 8 bits");
	}

	/*
	 * We need three temporary entry arrays for purge_search_list().
	 * Allocate them contiguously with the main array.
	 */
	bytes = sizeof(struct search_list) + (4 * capacity * sizeof(uint8_t));
	result = uds_allocate_cache_aligned(bytes, "search list", &list);
	if (result != UDS_SUCCESS) {
		return result;
	}

	list->capacity = capacity;
	list->first_dead_entry = 0;

	/*
	 * Fill in the indexes of the chapter index cache entries. These will
	 * be only ever be permuted as the search list is used.
	 */
	for (i = 0; i < capacity; i++) {
		list->entries[i] = i;
	}

	*list_ptr = list;
	return UDS_SUCCESS;
}

/**********************************************************************/
void purge_search_list(struct search_list *search_list,
		       const struct cached_chapter_index chapters[],
		       uint64_t oldest_virtual_chapter)
{
	uint8_t *entries, *alive, *skipped, *dead;
	unsigned int next_alive, next_skipped, next_dead;
	int i;

	if (search_list->first_dead_entry == 0) {
		/* There are no live entries in the list to purge. */
		return;
	}

	/*
	 * Partition the previously-alive entries in the list into three
	 * temporary lists, keeping the current LRU search order within each
	 * list. The element array was allocated with enough space for all four
	 * lists.
	 */
	entries = &search_list->entries[0];
	alive = &entries[search_list->capacity];
	skipped = &alive[search_list->capacity];
	dead = &skipped[search_list->capacity];
	next_alive = next_skipped = next_dead = 0;

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

	/*
	 * Copy the temporary lists back to the search list so we wind up with
	 * [ alive, alive, skippable, new-dead, new-dead, old-dead, old-dead ]
	 */
	memcpy(entries, alive, next_alive);
	entries += next_alive;

	memcpy(entries, skipped, next_skipped);
	entries += next_skipped;

	memcpy(entries, dead, next_dead);
	/* The first dead entry is now the start of the copied dead list. */
	search_list->first_dead_entry = (next_alive + next_skipped);
}
