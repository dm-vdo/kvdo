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

#include "indexPageMap.h"

#include "buffer.h"
#include "bufferedWriter.h"
#include "compiler.h"
#include "errors.h"
#include "hashUtils.h"
#include "indexComponent.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "stringUtils.h"
#include "uds-threads.h"
#include "uds.h"

static int read_index_page_map(struct read_portal *portal);
static int write_index_page_map(struct index_component *component,
				struct buffered_writer *writer,
				unsigned int zone);

static const byte INDEX_PAGE_MAP_MAGIC[] = "ALBIPM02";
enum {
	INDEX_PAGE_MAP_MAGIC_LENGTH = sizeof(INDEX_PAGE_MAP_MAGIC) - 1,
};

const struct index_component_info INDEX_PAGE_MAP_INFO = {
	.kind         = RL_KIND_INDEX_PAGE_MAP,
	.name         = "index page map",
	.multi_zone   = false,
	.io_storage   = true,
	.loader       = read_index_page_map,
	.saver        = write_index_page_map,
};

/**********************************************************************/
static INLINE size_t num_entries(const struct geometry *geometry)
{
	return geometry->chapters_per_volume *
	       (geometry->index_pages_per_chapter - 1);
}

/**********************************************************************/
int make_index_page_map(const struct geometry *geometry,
			struct index_page_map **map_ptr)
{
	struct index_page_map *map;
	unsigned int delta_lists_per_chapter =
		geometry->delta_lists_per_chapter;
	int result = ASSERT_WITH_ERROR_CODE(((delta_lists_per_chapter - 1) <=
						UINT16_MAX),
					    UDS_BAD_STATE,
					    "delta lists per chapter (%u) is too large",
					    delta_lists_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(1, struct index_page_map, "Index Page Map", &map);
	if (result != UDS_SUCCESS) {
		return result;
	}

	map->geometry = geometry;

	result = UDS_ALLOCATE(num_entries(geometry),
			      index_page_map_entry_t,
			      "Index Page Map Entries",
			      &map->entries);
	if (result != UDS_SUCCESS) {
		free_index_page_map(map);
		return result;
	}

	*map_ptr = map;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_index_page_map(struct index_page_map *map)
{
	if (map != NULL) {
		UDS_FREE(map->entries);
		UDS_FREE(map);
	}
}

/**********************************************************************/
uint64_t get_last_update(const struct index_page_map *map)
{
	return map->last_update;
}

/**********************************************************************/
int update_index_page_map(struct index_page_map *map,
			  uint64_t virtual_chapter_number,
			  unsigned int chapter_number,
			  unsigned int index_page_number,
			  unsigned int delta_list_number)
{
	size_t slot;
	const struct geometry *geometry = map->geometry;
	if ((virtual_chapter_number < map->last_update) ||
	    (virtual_chapter_number > map->last_update + 1)) {
		/*
		 * if the last_update is 0, this is likely to be normal because
		 * we are replaying the volume
		 */
		if (map->last_update != 0) {
			uds_log_warning("unexpected index page map update, jumping from %llu to %llu",
					(unsigned long long) map->last_update,
					(unsigned long long) virtual_chapter_number);
		}
	}
	map->last_update = virtual_chapter_number;

	if (chapter_number >= geometry->chapters_per_volume) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "chapter number %u exceeds maximum %u",
					      chapter_number,
					      geometry->chapters_per_volume - 1);
	}
	if (index_page_number >= geometry->index_pages_per_chapter) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "index page number %u exceeds maximum %u",
					      index_page_number,
					      geometry->index_pages_per_chapter - 1);
	}
	if (delta_list_number >= geometry->delta_lists_per_chapter) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "delta list number %u exceeds maximum %u",
					      delta_list_number,
					      geometry->delta_lists_per_chapter - 1);
	}

	if (index_page_number == (geometry->index_pages_per_chapter - 1)) {
		/*
		 * There is no entry for the last index page of a chapter since
		 * its entry would always be
		 * geometry->delta_lists_per_chapter - 1.
		 */
		return UDS_SUCCESS;
	}

	slot = (chapter_number * (geometry->index_pages_per_chapter - 1)) +
		index_page_number;
	map->entries[slot] = (index_page_map_entry_t) delta_list_number;
	return UDS_SUCCESS;
}

/**********************************************************************/
int find_index_page_number(const struct index_page_map *map,
			   const struct uds_chunk_name *name,
			   unsigned int chapter_number,
			   unsigned int *index_page_number_ptr)
{
	int result;
	unsigned int delta_list_number, slot, limit, index_page_number = 0;
	const struct geometry *geometry = map->geometry;
	if (chapter_number >= geometry->chapters_per_volume) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "chapter number %u exceeds maximum %u",
					      chapter_number,
					      geometry->chapters_per_volume - 1);
	}

	delta_list_number = hash_to_chapter_delta_list(name, geometry);
	slot = (chapter_number * (geometry->index_pages_per_chapter - 1));
	limit = slot + (geometry->index_pages_per_chapter - 1);
	for (; slot < limit; index_page_number++, slot++) {
		if (delta_list_number <= map->entries[slot]) {
			break;
		}
	}

	/*
	 * This should be a clear post-condition of the loop above, but just in
	 * case it's not obvious, the check is cheap.
	 */
	result =
		ASSERT((index_page_number < geometry->index_pages_per_chapter),
		       "index page number too large");
	if (result != UDS_SUCCESS) {
		return result;
	}

	*index_page_number_ptr = index_page_number;
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_list_number_bounds(const struct index_page_map *map,
			   unsigned int chapter_number,
			   unsigned int index_page_number,
			   struct index_page_bounds *bounds)
{
	unsigned int slot;
	const struct geometry *geometry = map->geometry;
	int result = ASSERT((chapter_number < geometry->chapters_per_volume),
			    "chapter number is valid");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT((index_page_number < geometry->index_pages_per_chapter),
			"index page number is valid");
	if (result != UDS_SUCCESS) {
		return result;
	}

	slot = chapter_number * (geometry->index_pages_per_chapter - 1);
	bounds->lowest_list =
		((index_page_number == 0) ?
			 0 :
			 map->entries[slot + index_page_number - 1] + 1);
	bounds->highest_list =
		((index_page_number == geometry->index_pages_per_chapter - 1) ?
			 geometry->delta_lists_per_chapter - 1 :
			 map->entries[slot + index_page_number]);

	return UDS_SUCCESS;
}

/**********************************************************************/
size_t index_page_map_size(const struct geometry *geometry)
{
	return sizeof(index_page_map_entry_t) * num_entries(geometry);
}

/**********************************************************************/
static int write_index_page_map(struct index_component *component,
				struct buffered_writer *writer,
				unsigned int zone)
{
	struct index_page_map *map;
	struct buffer *buffer;

	int result = ASSERT((zone == 0), "unimplemented zone %d", zone);
	if (result != UDS_SUCCESS) {
		return result;
	}

	map = index_component_data(component);

	result = make_buffer(INDEX_PAGE_MAP_MAGIC_LENGTH +
				     sizeof(map->last_update),
			     &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_bytes(buffer, INDEX_PAGE_MAP_MAGIC_LENGTH,
			   INDEX_PAGE_MAP_MAGIC);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, map->last_update);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(writer, get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot write index page map header");
	}

	result = make_buffer(index_page_map_size(map->geometry), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint16_les_into_buffer(buffer, num_entries(map->geometry),
					    map->entries);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(writer, get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot write index page map data");
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
uint64_t compute_index_page_map_save_size(const struct geometry *geometry)
{
	return index_page_map_size(geometry) + INDEX_PAGE_MAP_MAGIC_LENGTH +
	       sizeof(((struct index_page_map *) 0)->last_update);
}

/**********************************************************************/
static int __must_check decode_index_page_map(struct buffer *buffer,
					      struct index_page_map *map)
{
	int result = get_uint64_le_from_buffer(buffer, &map->last_update);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint16_les_from_buffer(buffer, num_entries(map->geometry),
					    map->entries);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
				 "%zu bytes decoded of %zu expected",
				 buffer_length(buffer) -
				 	content_length(buffer),
				 buffer_length(buffer));
	return result;
}

/**********************************************************************/
static int read_index_page_map(struct read_portal *portal)
{
	struct index_page_map *map = index_component_data(portal->component);
	struct buffer *buffer;
	struct buffered_reader *reader = NULL;

	int result = get_buffered_reader_for_portal(portal, 0, &reader);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = verify_buffered_data(reader, INDEX_PAGE_MAP_MAGIC,
				      INDEX_PAGE_MAP_MAGIC_LENGTH);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "bad index page map saved magic");
	}

	result = make_buffer(sizeof(map->last_update) +
				     index_page_map_size(map->geometry),
			     &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = read_from_buffered_reader(reader, get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		uds_log_error_strerror(result,
				       "cannot read index page map data");
		return result;
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = decode_index_page_map(buffer, map);
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}
	uds_log_debug("read index page map, last update %llu",
		      (unsigned long long) map->last_update);
	return UDS_SUCCESS;
}
