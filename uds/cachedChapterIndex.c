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
 * $Id: //eng/uds-releases/krusty/src/uds/cachedChapterIndex.c#14 $
 */

#include "cachedChapterIndex.h"

#include "memoryAlloc.h"

/**********************************************************************/
int initialize_cached_chapter_index(struct cached_chapter_index *chapter,
				    const struct geometry *geometry)
{
	int result;
	unsigned int i;
	chapter->virtual_chapter = UINT64_MAX;
	chapter->index_pages_count = geometry->index_pages_per_chapter;

	result = ALLOCATE(chapter->index_pages_count,
			  struct delta_index_page,
			  __func__,
			  &chapter->index_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ALLOCATE(chapter->index_pages_count,
			  struct volume_page,
			  "sparse index volume pages",
			  &chapter->volume_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < chapter->index_pages_count; i++) {
		result = initialize_volume_page(geometry,
						&chapter->volume_pages[i]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
void destroy_cached_chapter_index(struct cached_chapter_index *chapter)
{
	if (chapter->volume_pages != NULL) {
		unsigned int i;
		for (i = 0; i < chapter->index_pages_count; i++) {
			destroy_volume_page(&chapter->volume_pages[i]);
		}
	}
	FREE(chapter->index_pages);
	FREE(chapter->volume_pages);
}

/**********************************************************************/
int cache_chapter_index(struct cached_chapter_index *chapter,
			uint64_t virtual_chapter,
			const struct volume *volume)
{
	int result;
	// Mark the cached chapter as unused in case the update fails midway.
	chapter->virtual_chapter = UINT64_MAX;

	// Read all the page data and initialize the entire delta_index_page
	// array. (It's not safe for the zone threads to do it lazily--they'll
	// race.)
	result = read_chapter_index_from_volume(volume,
						virtual_chapter,
						chapter->volume_pages,
						chapter->index_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Reset all chapter counter values to zero.
	chapter->counters.search_hits = 0;
	chapter->counters.search_misses = 0;
	chapter->counters.consecutive_misses = 0;

	// Mark the entry as valid--it's now in the cache.
	chapter->virtual_chapter = virtual_chapter;
	chapter->skip_search = false;

	return UDS_SUCCESS;
}

/**********************************************************************/
int search_cached_chapter_index(struct cached_chapter_index *chapter,
				const struct geometry *geometry,
				const struct index_page_map *index_page_map,
				const struct uds_chunk_name *name,
				int *record_page_ptr)
{
	// Find the index_page_number in the chapter that would have the chunk
	// name.
	unsigned int physical_chapter =
		map_to_physical_chapter(geometry, chapter->virtual_chapter);
	unsigned int index_page_number;
	int result = find_index_page_number(index_page_map, name,
					    physical_chapter,
					    &index_page_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return search_chapter_index_page(&chapter->index_pages[index_page_number],
				         geometry,
				         name,
				         record_page_ptr);
}
