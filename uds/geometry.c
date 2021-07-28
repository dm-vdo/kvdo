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
 * $Id: //eng/uds-releases/krusty/src/uds/geometry.c#13 $
 */

#include "geometry.h"

#include "deltaIndex.h"
#include "errors.h"
#include "hashUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"

/**********************************************************************/
static int initialize_geometry(struct geometry *geometry,
			       size_t bytes_per_page,
			       unsigned int record_pages_per_chapter,
			       unsigned int chapters_per_volume,
			       unsigned int sparse_chapters_per_volume,
			       uint64_t remapped_virtual,
			       uint64_t remapped_physical)
{
	int result =
		ASSERT_WITH_ERROR_CODE(bytes_per_page >= BYTES_PER_RECORD,
				       UDS_BAD_STATE,
				       "page is smaller than a record: %zu",
				       bytes_per_page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT_WITH_ERROR_CODE(chapters_per_volume >
						sparse_chapters_per_volume,
					UDS_INVALID_ARGUMENT,
					"sparse chapters per volume (%u) must be less than chapters per volume (%u)",
					sparse_chapters_per_volume,
					chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}

	geometry->bytes_per_page = bytes_per_page;
	geometry->record_pages_per_chapter = record_pages_per_chapter;
	geometry->chapters_per_volume = chapters_per_volume;
	geometry->sparse_chapters_per_volume = sparse_chapters_per_volume;
	geometry->dense_chapters_per_volume =
		chapters_per_volume - sparse_chapters_per_volume;
	geometry->remapped_virtual = remapped_virtual;
	geometry->remapped_physical = remapped_physical;

	// Calculate the number of records in a page, chapter, and volume.
	geometry->records_per_page = bytes_per_page / BYTES_PER_RECORD;
	geometry->records_per_chapter =
		geometry->records_per_page * record_pages_per_chapter;
	geometry->records_per_volume =
		(unsigned long) geometry->records_per_chapter *
		chapters_per_volume;
	geometry->open_chapter_load_ratio = DEFAULT_OPEN_CHAPTER_LOAD_RATIO;

	// Initialize values for delta chapter indexes.
	geometry->chapter_mean_delta = 1 << DEFAULT_CHAPTER_MEAN_DELTA_BITS;
	geometry->chapter_payload_bits =
		compute_bits(record_pages_per_chapter - 1);
	// We want 1 delta list for every 64 records in the chapter.
	// The "| 077" ensures that the chapter_delta_list_bits computation
	// does not underflow.
	geometry->chapter_delta_list_bits =
		compute_bits((geometry->records_per_chapter - 1) | 077) - 6;
	geometry->delta_lists_per_chapter =
		1 << geometry->chapter_delta_list_bits;
	// We need enough address bits to achieve the desired mean delta.
	geometry->chapter_address_bits =
		(DEFAULT_CHAPTER_MEAN_DELTA_BITS -
		 geometry->chapter_delta_list_bits +
		 compute_bits(geometry->records_per_chapter - 1));
	// Let the delta index code determine how many pages are needed for the
	// index
	geometry->index_pages_per_chapter =
		get_delta_index_page_count(geometry->records_per_chapter,
					   geometry->delta_lists_per_chapter,
					   geometry->chapter_mean_delta,
					   geometry->chapter_payload_bits,
					   bytes_per_page);

	// Now that we have the size of a chapter index, we can calculate the
	// space used by chapters and volumes.
	geometry->pages_per_chapter =
		geometry->index_pages_per_chapter + record_pages_per_chapter;
	geometry->pages_per_volume =
		geometry->pages_per_chapter * chapters_per_volume;
	geometry->header_pages_per_volume = 1;
	geometry->bytes_per_volume =
		bytes_per_page * (geometry->pages_per_volume +
				  geometry->header_pages_per_volume);
	geometry->bytes_per_chapter =
		bytes_per_page * geometry->pages_per_chapter;

	return UDS_SUCCESS;
}

/**********************************************************************/
int make_geometry(size_t bytes_per_page,
		  unsigned int record_pages_per_chapter,
		  unsigned int chapters_per_volume,
		  unsigned int sparse_chapters_per_volume,
		  uint64_t remapped_virtual,
		  uint64_t remapped_physical,
		  struct geometry **geometry_ptr)
{
	struct geometry *geometry;
	int result = UDS_ALLOCATE(1, struct geometry, "geometry", &geometry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = initialize_geometry(geometry,
				     bytes_per_page,
				     record_pages_per_chapter,
				     chapters_per_volume,
				     sparse_chapters_per_volume,
				     remapped_virtual,
				     remapped_physical);
	if (result != UDS_SUCCESS) {
		free_geometry(geometry);
		return result;
	}

	*geometry_ptr = geometry;
	return UDS_SUCCESS;
}

/**********************************************************************/
int copy_geometry(struct geometry *source, struct geometry **geometry_ptr)
{
	return make_geometry(source->bytes_per_page,
			     source->record_pages_per_chapter,
			     source->chapters_per_volume,
			     source->sparse_chapters_per_volume,
			     source->remapped_virtual,
			     source->remapped_physical,
			     geometry_ptr);
}

/**********************************************************************/
void free_geometry(struct geometry *geometry)
{
	UDS_FREE(geometry);
}

/**********************************************************************/
unsigned int __must_check
map_to_physical_chapter(const struct geometry *geometry,
			uint64_t virtual_chapter)
{
	uint64_t delta;
	if (!is_reduced_geometry(geometry)) {
		return (virtual_chapter % geometry->chapters_per_volume);
	}

	if (likely(virtual_chapter > geometry->remapped_virtual)) {
		delta = virtual_chapter - geometry->remapped_virtual;
		if (likely(delta > geometry->remapped_physical)) {
			return (delta % geometry->chapters_per_volume);
		} else {
			return (delta - 1);
		}
	}

	if (virtual_chapter == geometry->remapped_virtual) {
		return geometry->remapped_physical;
	}

	delta = geometry->remapped_virtual - virtual_chapter;
	if (delta < geometry->chapters_per_volume) {
		return (geometry->chapters_per_volume - delta);
	}

	// This chapter is so old the answer doesn't matter.
	return 0;
}

/**********************************************************************/
bool has_sparse_chapters(const struct geometry *geometry,
			 uint64_t oldest_virtual_chapter,
			 uint64_t newest_virtual_chapter)
{
	return (is_sparse(geometry) &&
		((newest_virtual_chapter - oldest_virtual_chapter + 1) >
		 geometry->dense_chapters_per_volume));
}

/**********************************************************************/
bool is_chapter_sparse(const struct geometry *geometry,
		       uint64_t oldest_virtual_chapter,
		       uint64_t newest_virtual_chapter,
		       uint64_t virtual_chapter_number)
{
	return (has_sparse_chapters(geometry,
				    oldest_virtual_chapter,
				    newest_virtual_chapter) &&
		((virtual_chapter_number +
		  geometry->dense_chapters_per_volume) <=
		 newest_virtual_chapter));
}

/**********************************************************************/
unsigned int chapters_to_expire(const struct geometry *geometry,
				uint64_t newest_chapter)
{
	// If the index isn't full yet, don't expire anything.
	if (newest_chapter < geometry->chapters_per_volume) {
		return 0;
	}

	// If a chapter is out of order...
	if (geometry->remapped_physical > 0) {
		uint64_t oldest_chapter =
			newest_chapter - geometry->chapters_per_volume;

		// ... expire an extra chapter when expiring the moved chapter
		// to free physical space for the new chapter ...
		if (oldest_chapter == geometry->remapped_virtual) {
			return 2;
		}

		// ... but don't expire anything when the new chapter will use
		// the physical chapter freed by expiring the moved chapter.
		if (oldest_chapter == (geometry->remapped_virtual +
				       geometry->remapped_physical)) {
			return 0;
		}
	}

	// Normally, just expire one.
	return 1;
}
