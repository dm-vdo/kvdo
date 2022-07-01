// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "index-page-map.h"

#include "buffer.h"
#include "compiler.h"
#include "errors.h"
#include "hash-utils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "string-utils.h"
#include "uds-threads.h"
#include "uds.h"

/*
 *  Each volume maintains an index page map which records how the chapter delta
 *  lists are distributed among the index pages for that chapter.
 *
 *  The map is conceptually a two-dimensional array indexed by chapter number
 *  and index page number within the chapter. Each entry contains the number
 *  of the last delta list on that index page. In order to save memory, the
 *  information for the last page in each chapter is not recorded, as it is
 *  known from the geometry.
 */

static const byte PAGE_MAP_MAGIC[] = "ALBIPM02";

enum {
	PAGE_MAP_MAGIC_LENGTH = sizeof(PAGE_MAP_MAGIC) - 1,
};

static INLINE size_t get_entry_count(const struct geometry *geometry)
{
	return (geometry->chapters_per_volume *
		(geometry->index_pages_per_chapter - 1));
}

int make_index_page_map(const struct geometry *geometry,
			struct index_page_map **map_ptr)
{
	int result;
	struct index_page_map *map;

	result = UDS_ALLOCATE(1, struct index_page_map, "page map", &map);
	if (result != UDS_SUCCESS) {
		return result;
	}

	map->geometry = geometry;
	map->entries_per_chapter = geometry->index_pages_per_chapter - 1;
	result = UDS_ALLOCATE(get_entry_count(geometry),
			      uint16_t,
			      "Index Page Map Entries",
			      &map->entries);
	if (result != UDS_SUCCESS) {
		free_index_page_map(map);
		return result;
	}

	*map_ptr = map;
	return UDS_SUCCESS;
}

void free_index_page_map(struct index_page_map *map)
{
	if (map != NULL) {
		UDS_FREE(map->entries);
		UDS_FREE(map);
	}
}

void update_index_page_map(struct index_page_map *map,
			   uint64_t virtual_chapter_number,
			   unsigned int chapter_number,
			   unsigned int index_page_number,
			   unsigned int delta_list_number)
{
	size_t slot;

	map->last_update = virtual_chapter_number;
	if (index_page_number == map->entries_per_chapter) {
		return;
	}

	slot = (chapter_number * map->entries_per_chapter) + index_page_number;
	map->entries[slot] = delta_list_number;
}

unsigned int find_index_page_number(const struct index_page_map *map,
				    const struct uds_chunk_name *name,
				    unsigned int chapter_number)
{
	unsigned int delta_list_number =
		hash_to_chapter_delta_list(name, map->geometry);
	unsigned int slot = chapter_number * map->entries_per_chapter;
	unsigned int page;

	for (page = 0; page < map->entries_per_chapter; page++) {
		if (delta_list_number <= map->entries[slot + page]) {
			break;
		}
	}

	return page;
}

void get_list_number_bounds(const struct index_page_map *map,
			    unsigned int chapter_number,
			    unsigned int index_page_number,
			    unsigned int *lowest_list,
			    unsigned int *highest_list)
{
	unsigned int slot = chapter_number * map->entries_per_chapter;

	*lowest_list = ((index_page_number == 0) ?
			0 :
			map->entries[slot + index_page_number - 1] + 1);
	*highest_list = ((index_page_number < map->entries_per_chapter) ?
			 map->entries[slot + index_page_number] :
			 map->geometry->delta_lists_per_chapter - 1);
}

uint64_t compute_index_page_map_save_size(const struct geometry *geometry)
{
	return (PAGE_MAP_MAGIC_LENGTH + sizeof(uint64_t) +
		sizeof(uint16_t) * get_entry_count(geometry));
}

int write_index_page_map(struct index_page_map *map,
			 struct buffered_writer *writer)
{
	int result;
	struct buffer *buffer;

	result = make_buffer(compute_index_page_map_save_size(map->geometry),
			     &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_bytes(buffer, PAGE_MAP_MAGIC_LENGTH, PAGE_MAP_MAGIC);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, map->last_update);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = put_uint16_les_into_buffer(buffer,
					    get_entry_count(map->geometry),
					    map->entries);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	return flush_buffered_writer(writer);
}

int read_index_page_map(struct index_page_map *map,
			struct buffered_reader *reader)
{
	int result;
	struct buffer *buffer;
	byte magic[PAGE_MAP_MAGIC_LENGTH];

	result = make_buffer(compute_index_page_map_save_size(map->geometry),
			     &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = get_bytes_from_buffer(buffer, PAGE_MAP_MAGIC_LENGTH, &magic);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	if (memcmp(magic, PAGE_MAP_MAGIC, PAGE_MAP_MAGIC_LENGTH) != 0) {
		free_buffer(UDS_FORGET(buffer));
		return UDS_CORRUPT_DATA;
	}

	result = get_uint64_le_from_buffer(buffer, &map->last_update);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = get_uint16_les_from_buffer(buffer,
					    get_entry_count(map->geometry),
					    map->entries);
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	uds_log_debug("read index page map, last update %llu",
		      (unsigned long long) map->last_update);
	return UDS_SUCCESS;
}
