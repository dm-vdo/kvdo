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
 * $Id: //eng/uds-releases/krusty/src/uds/chapterIndex.c#3 $
 */

#include "chapterIndex.h"

#include "compiler.h"
#include "errors.h"
#include "hashUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"


/**********************************************************************/
int make_open_chapter_index(struct open_chapter_index **open_chapter_index,
			    const Geometry *geometry,
			    bool chapter_index_header_native_endian,
			    uint64_t volume_nonce)
{

	int result = ALLOCATE(1,
			      struct open_chapter_index,
			      "open chapter index",
			      open_chapter_index);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// The delta index will rebalance delta lists when memory gets tight,
	// so give the chapter index one extra page.
	size_t memory_size =
		(geometry->indexPagesPerChapter + 1) * geometry->bytesPerPage;
	(*open_chapter_index)->geometry = geometry;
	(*open_chapter_index)->volume_nonce = volume_nonce;
	(*open_chapter_index)->header_native_endian =
		chapter_index_header_native_endian;
	result = initializeDeltaIndex(&(*open_chapter_index)->delta_index,
				      1,
				      geometry->deltaListsPerChapter,
				      geometry->chapterMeanDelta,
				      geometry->chapterPayloadBits,
				      memory_size);
	if (result != UDS_SUCCESS) {
		FREE(*open_chapter_index);
		*open_chapter_index = NULL;
	}
	return result;
}

/**********************************************************************/
void free_open_chapter_index(struct open_chapter_index *open_chapter_index)
{
	if (open_chapter_index == NULL) {
		return;
	}


	uninitializeDeltaIndex(&open_chapter_index->delta_index);
	FREE(open_chapter_index);
}

/**********************************************************************/
void empty_open_chapter_index(struct open_chapter_index *open_chapter_index,
			      uint64_t virtual_chapter_number)
{
	emptyDeltaIndex(&open_chapter_index->delta_index);
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
static INLINE bool was_entry_found(const DeltaIndexEntry *entry,
				   unsigned int address)
{
	return (!entry->atEnd && (entry->key == address));
}

/**********************************************************************/
int put_open_chapter_index_record(struct open_chapter_index *open_chapter_index,
				  const UdsChunkName *name,
				  unsigned int page_number)
{
	const Geometry *geometry = open_chapter_index->geometry;
	int result = ASSERT_WITH_ERROR_CODE(page_number <
						geometry->recordPagesPerChapter,
					    UDS_INVALID_ARGUMENT,
					    "Page number within chapter (%u) exceeds the maximum value %u",
					    page_number,
					    geometry->recordPagesPerChapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	DeltaIndexEntry entry;
	unsigned int address = hashToChapterDeltaAddress(name, geometry);
	result = getDeltaIndexEntry(&open_chapter_index->delta_index,
				    hashToChapterDeltaList(name, geometry),
				    address,
				    name->name,
				    false,
				    &entry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	bool found = was_entry_found(&entry, address);
	result = ASSERT_WITH_ERROR_CODE(!(found && entry.isCollision),
					UDS_BAD_STATE,
					"Chunk appears more than once in chapter %llu",
					open_chapter_index->virtual_chapter_number);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return putDeltaIndexEntry(&entry, address, page_number,
				  (found ? name->name : NULL));
}

/**********************************************************************/
int pack_open_chapter_index_page(struct open_chapter_index *open_chapter_index,
				 byte *memory,
				 unsigned int first_list,
				 bool last_page,
				 unsigned int *num_lists)
{
	DeltaIndex *delta_index = &open_chapter_index->delta_index;
	const Geometry *geometry = open_chapter_index->geometry;
	unsigned int removals = 0;
	for (;;) {
		int result =
			packDeltaIndexPage(delta_index,
					   open_chapter_index->volume_nonce,
					   open_chapter_index->header_native_endian,
					   memory,
					   geometry->bytesPerPage,
					   open_chapter_index->virtual_chapter_number,
					   first_list,
					   num_lists);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if ((first_list + *num_lists) ==
		    geometry->deltaListsPerChapter) {
			// All lists are packed
			break;
		} else if (*num_lists == 0) {
			// The next delta list does not fit on a page.  This
			// delta list will be removed.
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
			// This page is done
			break;
		}
		if (removals == 0) {
			DeltaIndexStats stats;
			getDeltaIndexStats(delta_index, &stats);
			logWarning("The chapter index for chapter %" PRIu64
				   " contains %ld entries with %ld collisions",
				   open_chapter_index->virtual_chapter_number,
				   stats.recordCount,
				   stats.collisionCount);
		}
		DeltaIndexEntry entry;
		int list_number = *num_lists;
		do {
			if (list_number < 0) {
				return UDS_OVERFLOW;
			}
			result = startDeltaIndexSearch(delta_index,
						       first_list +
							       list_number--,
						       0,
						       false,
						       &entry);
			if (result != UDS_SUCCESS) {
				return result;
			}
			result = nextDeltaIndexEntry(&entry);
			if (result != UDS_SUCCESS) {
				return result;
			}
		} while (entry.atEnd);
		do {
			result = removeDeltaIndexEntry(&entry);
			if (result != UDS_SUCCESS) {
				return result;
			}
			removals++;
		} while (!entry.atEnd);
	}
	if (removals > 0) {
		logWarning("To avoid chapter index page overflow in chapter %llu, %u entries were removed from the chapter index",
			   open_chapter_index->virtual_chapter_number,
			   removals);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_open_chapter_index_size(struct open_chapter_index *open_chapter_index)
{
	DeltaIndexStats stats;
	getDeltaIndexStats(&open_chapter_index->delta_index, &stats);
	return stats.recordCount;
}

/**********************************************************************/
size_t
get_open_chapter_index_memory_allocated(struct open_chapter_index *open_chapter_index)
{
	DeltaIndexStats stats;
	getDeltaIndexStats(&open_chapter_index->delta_index, &stats);
	return stats.memoryAllocated + sizeof(struct open_chapter_index);
}

/**********************************************************************/
int initialize_chapter_index_page(DeltaIndexPage *chapter_index_page,
				  const Geometry *geometry,
				  byte *index_page,
				  uint64_t volume_nonce)
{
	return initializeDeltaIndexPage(chapter_index_page,
					volume_nonce,
					geometry->chapterMeanDelta,
					geometry->chapterPayloadBits,
					index_page,
					geometry->bytesPerPage);
}

/**********************************************************************/
int validate_chapter_index_page(const DeltaIndexPage *chapter_index_page,
				const Geometry *geometry)
{
	const DeltaIndex *delta_index = &chapter_index_page->deltaIndex;
	unsigned int first = chapter_index_page->lowestListNumber;
	unsigned int last = chapter_index_page->highestListNumber;
	// We walk every delta list from start to finish.
	unsigned int list_number;
	for (list_number = first; list_number <= last; list_number++) {
		DeltaIndexEntry entry;
		int result =
			startDeltaIndexSearch(delta_index, list_number - first,
					      0, true, &entry);
		if (result != UDS_SUCCESS) {
			return result;
		}
		for (;;) {
			result = nextDeltaIndexEntry(&entry);
			if (result != UDS_SUCCESS) {
				if (result == UDS_CORRUPT_DATA) {
					// A random bit stream is highly likely
					// to arrive here when we go past the
					// end of the delta list
					return UDS_CORRUPT_COMPONENT;
				}
				return result;
			}
			if (entry.atEnd) {
				break;
			}
			// Also make sure that the record page field contains a
			// plausible value
			if (getDeltaEntryValue(&entry) >=
			    geometry->recordPagesPerChapter) {
				// Do not log this as an error.  It happens in
				// normal operation when we are doing a rebuild
				// but haven't written the entire volume once.
				return UDS_CORRUPT_COMPONENT;
			}
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int search_chapter_index_page(DeltaIndexPage *chapter_index_page,
			      const Geometry *geometry,
			      const UdsChunkName *name,
			      int *record_page_ptr)
{
	DeltaIndex *delta_index = &chapter_index_page->deltaIndex;
	unsigned int address = hashToChapterDeltaAddress(name, geometry);
	unsigned int delta_list_number = hashToChapterDeltaList(name, geometry);
	unsigned int sub_list_number =
		delta_list_number - chapter_index_page->lowestListNumber;
	DeltaIndexEntry entry;
	int result =
		getDeltaIndexEntry(delta_index, sub_list_number, address,
				   name->name, true, &entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (was_entry_found(&entry, address)) {
		*record_page_ptr = getDeltaEntryValue(&entry);
	} else {
		*record_page_ptr = NO_CHAPTER_INDEX_ENTRY;
	}
	return UDS_SUCCESS;
}
