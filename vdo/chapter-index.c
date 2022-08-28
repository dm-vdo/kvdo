// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "chapter-index.h"

#include "compiler.h"
#include "errors.h"
#include "hash-utils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds.h"

int make_open_chapter_index(struct open_chapter_index **chapter_index,
			    const struct geometry *geometry,
			    uint64_t volume_nonce)
{
	int result;
	size_t memory_size;
	struct delta_index_stats stats;
	struct open_chapter_index *index;

	result = UDS_ALLOCATE(1,
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
	memory_size = ((geometry->index_pages_per_chapter + 1) *
		       geometry->bytes_per_page);
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
	*chapter_index = index;
	return UDS_SUCCESS;
}

void free_open_chapter_index(struct open_chapter_index *chapter_index)
{
	if (chapter_index == NULL) {
		return;
	}

	uninitialize_delta_index(&chapter_index->delta_index);
	UDS_FREE(chapter_index);
}

/* Re-initialize an open chapter index for a new chapter. */
void empty_open_chapter_index(struct open_chapter_index *chapter_index,
			      uint64_t virtual_chapter_number)
{
	empty_delta_index(&chapter_index->delta_index);
	chapter_index->virtual_chapter_number = virtual_chapter_number;
}

static INLINE bool was_entry_found(const struct delta_index_entry *entry,
				   unsigned int address)
{
	return (!entry->at_end && (entry->key == address));
}

/* Associate a chunk name with the record page containing its metadata. */
int put_open_chapter_index_record(struct open_chapter_index *chapter_index,
				  const struct uds_chunk_name *name,
				  unsigned int page_number)
{
	int result;
	struct delta_index_entry entry;
	unsigned int address;
	unsigned int list_number;
	const byte *found_name;
	bool found;
	const struct geometry *geometry = chapter_index->geometry;
	unsigned int chapter_number = chapter_index->virtual_chapter_number;
	unsigned int record_pages = geometry->record_pages_per_chapter;

	result = ASSERT_WITH_ERROR_CODE(page_number < record_pages,
					UDS_INVALID_ARGUMENT,
					"Page number within chapter (%u) exceeds the maximum value %u",
					page_number,
					record_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	address = hash_to_chapter_delta_address(name, geometry);
	list_number = hash_to_chapter_delta_list(name, geometry);
	result = get_delta_index_entry(&chapter_index->delta_index,
				       list_number,
				       address,
				       name->name,
				       &entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	found = was_entry_found(&entry, address);
	result = ASSERT_WITH_ERROR_CODE(!(found && entry.is_collision),
					UDS_BAD_STATE,
					"Chunk appears more than once in chapter %llu",
					(unsigned long long) chapter_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	found_name = (found ? name->name : NULL);
	return put_delta_index_entry(&entry, address, page_number, found_name);
}

/*
 * Pack a section of an open chapter index into a chapter index page. A
 * range of delta lists (starting with a specified list index) is copied
 * from the open chapter index into a memory page. The number of lists
 * copied onto the page is returned to the caller on success.
 *
 * @param chapter_index  The open chapter index
 * @param memory         The memory page to use
 * @param first_list     The first delta list number to be copied
 * @param last_page      If true, this is the last page of the chapter index
 *                       and all the remaining lists must be packed onto this
 *                       page
 * @param num_lists      The number of delta lists that were copied
 **/
int pack_open_chapter_index_page(struct open_chapter_index *chapter_index,
				 byte *memory,
				 unsigned int first_list,
				 bool last_page,
				 unsigned int *num_lists)
{
	int result;
	struct delta_index *delta_index = &chapter_index->delta_index;
	struct delta_index_stats stats;
	uint64_t nonce = chapter_index->volume_nonce;
	uint64_t chapter_number = chapter_index->virtual_chapter_number;
	const struct geometry *geometry = chapter_index->geometry;
	unsigned int list_count = geometry->delta_lists_per_chapter;
	unsigned int removals = 0;
	struct delta_index_entry entry;
	unsigned int next_list;
	int list_number;

	for (;;) {
		result = pack_delta_index_page(delta_index,
					       nonce,
					       memory,
					       geometry->bytes_per_page,
					       chapter_number,
					       first_list,
					       num_lists);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if ((first_list + *num_lists) == list_count) {
			/* All lists are packed. */
			break;
		} else if (*num_lists == 0) {
			/*
			 * The next delta list does not fit on a page. This
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
			/* This page is done. */
			break;
		}

		if (removals == 0) {
			get_delta_index_stats(delta_index, &stats);
			uds_log_warning("The chapter index for chapter %llu contains %ld entries with %ld collisions",
					(unsigned long long) chapter_number,
					stats.record_count,
					stats.collision_count);
		}

		list_number = *num_lists;
		do {
			if (list_number < 0) {
				return UDS_OVERFLOW;
			}

			next_list = first_list + list_number--,
			result = start_delta_index_search(delta_index,
							  next_list,
						          0,
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
				(unsigned long long) chapter_number,
				removals);
	}

	return UDS_SUCCESS;
}

/*
 * Make a new chapter index page, initializing it with the data from the
 * given index_page buffer.
 */
int initialize_chapter_index_page(struct delta_index_page *index_page,
				  const struct geometry *geometry,
				  byte *page_buffer,
				  uint64_t volume_nonce)
{
	return initialize_delta_index_page(index_page,
					   volume_nonce,
					   geometry->chapter_mean_delta,
					   geometry->chapter_payload_bits,
					   page_buffer,
					   geometry->bytes_per_page);
}

/* Validate a chapter index page read during rebuild. */
int validate_chapter_index_page(const struct delta_index_page *index_page,
				const struct geometry *geometry)
{
	int result;
	const struct delta_index *delta_index = &index_page->delta_index;
	unsigned int first = index_page->lowest_list_number;
	unsigned int last = index_page->highest_list_number;
	unsigned int list_number;

	/* We walk every delta list from start to finish. */
	for (list_number = first; list_number <= last; list_number++) {
		struct delta_index_entry entry;
		result = start_delta_index_search(delta_index,
						  list_number - first,
					          0,
						  &entry);
		if (result != UDS_SUCCESS) {
			return result;
		}

		for (;;) {
			result = next_delta_index_entry(&entry);
			if (result != UDS_SUCCESS) {
				/*
				 * A random bit stream is highly likely
				 * to arrive here when we go past the
				 * end of the delta list.
				 */
				return result;
			}

			if (entry.at_end) {
				break;
			}

			/*
			 * Also make sure that the record page field contains a
			 * plausible value.
			 */
			if (get_delta_entry_value(&entry) >=
			    geometry->record_pages_per_chapter) {
				/*
				 * Do not log this as an error. It happens in
				 * normal operation when we are doing a rebuild
				 * but haven't written the entire volume once.
				 */
				return UDS_CORRUPT_DATA;
			}
		}
	}
	return UDS_SUCCESS;
}

/*
 * Search a chapter index page for a chunk name, returning the record page
 * number that may contain the name.
 */
int search_chapter_index_page(struct delta_index_page *index_page,
			      const struct geometry *geometry,
			      const struct uds_chunk_name *name,
			      int *record_page_ptr)
{
	int result;
	struct delta_index *delta_index = &index_page->delta_index;
	unsigned int address = hash_to_chapter_delta_address(name, geometry);
	unsigned int delta_list_number =
		hash_to_chapter_delta_list(name, geometry);
	unsigned int sub_list_number =
		delta_list_number - index_page->lowest_list_number;
	struct delta_index_entry entry;

	result = get_delta_index_entry(delta_index,
				       sub_list_number,
				       address,
				       name->name,
				       &entry);
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
