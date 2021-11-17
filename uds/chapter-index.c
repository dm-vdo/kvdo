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

#include "chapter-index.h"

#include "compiler.h"
#include "errors.h"
#include "hash-utils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds.h"


/**********************************************************************/
int make_open_chapter_index(struct open_chapter_index **open_chapter_index,
			    const struct geometry *geometry,
			    uint64_t volume_nonce)
{
	size_t memory_size;
	struct delta_index_stats stats;
	struct open_chapter_index *index;

	int result = UDS_ALLOCATE(1,
				  struct open_chapter_index,
				  "open chapter index",
				  &index);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * The delta index will rebalance delta lists when memory gets tight,
	 * so give the chapter index one extra page.
	 */
	memory_size = (geometry->index_pages_per_chapter + 1) *
			geometry->bytes_per_page;
	index->geometry = geometry;
	index->volume_nonce = volume_nonce;
	result = initialize_delta_index(&index->delta_index,
				        1,
				        geometry->delta_lists_per_chapter,
				        geometry->chapter_mean_delta,
				        geometry->chapter_payload_bits,
				        memory_size);
	if (result != UDS_SUCCESS) {
		UDS_FREE(index);
		return result;
	}

	get_delta_index_stats(&index->delta_index, &stats);
	index->memory_allocated =
		stats.memory_allocated + sizeof(struct open_chapter_index);
	*open_chapter_index = index;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_open_chapter_index(struct open_chapter_index *open_chapter_index)
{
	if (open_chapter_index == NULL) {
		return;
	}


	uninitialize_delta_index(&open_chapter_index->delta_index);
	UDS_FREE(open_chapter_index);
}

/**********************************************************************/
void empty_open_chapter_index(struct open_chapter_index *open_chapter_index,
			      uint64_t virtual_chapter_number)
{
	empty_delta_index(&open_chapter_index->delta_index);
	open_chapter_index->virtual_chapter_number = virtual_chapter_number;
}

/**
 * Check whether a delta list entry reflects a successful search for a given
 * address.
 *
 * @param entry    the delta list entry from the search
 * @param address  the address of the desired entry
 *
 * @return <code>true</code> iff the address was found
 **/
static INLINE bool was_entry_found(const struct delta_index_entry *entry,
				   unsigned int address)
{
	return (!entry->at_end && (entry->key == address));
}

/**********************************************************************/
int put_open_chapter_index_record(struct open_chapter_index *open_chapter_index,
				  const struct uds_chunk_name *name,
				  unsigned int page_number)
{
	struct delta_index_entry entry;
	unsigned int address;
	bool found;
	const struct geometry *geometry = open_chapter_index->geometry;
	int result =
		ASSERT_WITH_ERROR_CODE(page_number <
						geometry->record_pages_per_chapter,
				       UDS_INVALID_ARGUMENT,
				       "Page number within chapter (%u) exceeds the maximum value %u",
				       page_number,
				       geometry->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	address = hash_to_chapter_delta_address(name, geometry);
	result = get_delta_index_entry(&open_chapter_index->delta_index,
				       hash_to_chapter_delta_list(name,
				       				  geometry),
				       address,
				       name->name,
				       false,
				       &entry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	found = was_entry_found(&entry, address);
	result = ASSERT_WITH_ERROR_CODE(!(found && entry.is_collision),
					UDS_BAD_STATE,
					"Chunk appears more than once in chapter %llu",
					(unsigned long long) open_chapter_index->virtual_chapter_number);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return put_delta_index_entry(&entry, address, page_number,
				     (found ? name->name : NULL));
}

/**********************************************************************/
int pack_open_chapter_index_page(struct open_chapter_index *open_chapter_index,
				 byte *memory,
				 unsigned int first_list,
				 bool last_page,
				 unsigned int *num_lists)
{
	struct delta_index *delta_index = &open_chapter_index->delta_index;
	const struct geometry *geometry = open_chapter_index->geometry;
	unsigned int removals = 0;
	struct delta_index_entry entry;
	int list_number;
	for (;;) {
		int result =
			pack_delta_index_page(delta_index,
					      open_chapter_index->volume_nonce,
					      memory,
					      geometry->bytes_per_page,
					      open_chapter_index->virtual_chapter_number,
					      first_list,
					      num_lists);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if ((first_list + *num_lists) ==
		    geometry->delta_lists_per_chapter) {
			/* All lists are packed */
			break;
		} else if (*num_lists == 0) {
			/*
			 * The next delta list does not fit on a page.  This
			 * delta list will be removed.
			 */
		} else if (last_page) {
			/*
			 * This is the last page and there are lists left
			 * unpacked, but all of the remaining lists must fit on
			 * the page. Find a list that contains entries and
			 * remove the entire list. Try the first list that does
			 * not fit. If it is empty, we will select the last list
			 * that already fits and has any entries.
			 */
		} else {
			/* This page is done */
			break;
		}
		if (removals == 0) {
			struct delta_index_stats stats;
			get_delta_index_stats(delta_index, &stats);
			uds_log_warning("The chapter index for chapter %llu contains %ld entries with %ld collisions",
					(unsigned long long) open_chapter_index->virtual_chapter_number,
					stats.record_count,
					stats.collision_count);
		}

		list_number = *num_lists;
		do {
			if (list_number < 0) {
				return UDS_OVERFLOW;
			}
			result = start_delta_index_search(delta_index,
							  first_list +
							       list_number--,
						          0,
						          false,
						          &entry);
			if (result != UDS_SUCCESS) {
				return result;
			}
			result = next_delta_index_entry(&entry);
			if (result != UDS_SUCCESS) {
				return result;
			}
		} while (entry.at_end);
		do {
			result = remove_delta_index_entry(&entry);
			if (result != UDS_SUCCESS) {
				return result;
			}
			removals++;
		} while (!entry.at_end);
	}
	if (removals > 0) {
		uds_log_warning("To avoid chapter index page overflow in chapter %llu, %u entries were removed from the chapter index",
				(unsigned long long) open_chapter_index->virtual_chapter_number,
				removals);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_open_chapter_index_size(struct open_chapter_index *open_chapter_index)
{
	struct delta_index_stats stats;
	get_delta_index_stats(&open_chapter_index->delta_index, &stats);
	return stats.record_count;
}

/**********************************************************************/
int initialize_chapter_index_page(struct delta_index_page *chapter_index_page,
				  const struct geometry *geometry,
				  byte *index_page,
				  uint64_t volume_nonce)
{
	return initialize_delta_index_page(chapter_index_page,
					   volume_nonce,
					   geometry->chapter_mean_delta,
					   geometry->chapter_payload_bits,
					   index_page,
					   geometry->bytes_per_page);
}

/**********************************************************************/
int validate_chapter_index_page(const struct delta_index_page *chapter_index_page,
				const struct geometry *geometry)
{
	const struct delta_index *delta_index = &chapter_index_page->delta_index;
	unsigned int first = chapter_index_page->lowest_list_number;
	unsigned int last = chapter_index_page->highest_list_number;
	/* We walk every delta list from start to finish. */
	unsigned int list_number;
	for (list_number = first; list_number <= last; list_number++) {
		struct delta_index_entry entry;
		int result =
			start_delta_index_search(delta_index,
						 list_number - first,
					         0, true, &entry);
		if (result != UDS_SUCCESS) {
			return result;
		}
		for (;;) {
			result = next_delta_index_entry(&entry);
			if (result != UDS_SUCCESS) {
				if (result == UDS_CORRUPT_DATA) {
					/*
					 * A random bit stream is highly likely
					 * to arrive here when we go past the
					 * end of the delta list
					 */
					return UDS_CORRUPT_COMPONENT;
				}
				return result;
			}
			if (entry.at_end) {
				break;
			}
			/*
			 * Also make sure that the record page field contains a
			 * plausible value
			 */
			if (get_delta_entry_value(&entry) >=
			    geometry->record_pages_per_chapter) {
				/*
				 * Do not log this as an error.  It happens in
				 * normal operation when we are doing a rebuild
				 * but haven't written the entire volume once.
				 */
				return UDS_CORRUPT_COMPONENT;
			}
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int search_chapter_index_page(struct delta_index_page *chapter_index_page,
			      const struct geometry *geometry,
			      const struct uds_chunk_name *name,
			      int *record_page_ptr)
{
	struct delta_index *delta_index = &chapter_index_page->delta_index;
	unsigned int address = hash_to_chapter_delta_address(name, geometry);
	unsigned int delta_list_number =
		hash_to_chapter_delta_list(name, geometry);
	unsigned int sub_list_number =
		delta_list_number - chapter_index_page->lowest_list_number;
	struct delta_index_entry entry;
	int result =
		get_delta_index_entry(delta_index, sub_list_number, address,
				      name->name, true, &entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (was_entry_found(&entry, address)) {
		*record_page_ptr = get_delta_entry_value(&entry);
	} else {
		*record_page_ptr = NO_CHAPTER_INDEX_ENTRY;
	}
	return UDS_SUCCESS;
}
