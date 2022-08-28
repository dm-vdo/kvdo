/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INDEX_PAGE_MAP_H
#define INDEX_PAGE_MAP_H 1

#include "buffered-reader.h"
#include "buffered-writer.h"
#include "common.h"
#include "geometry.h"

struct index_page_map {
	const struct geometry *geometry;
	uint64_t last_update;
	unsigned int entries_per_chapter;
	uint16_t *entries;
};

int __must_check make_index_page_map(const struct geometry *geometry,
				     struct index_page_map **map_ptr);

void free_index_page_map(struct index_page_map *map);

int __must_check read_index_page_map(struct index_page_map *map,
				     struct buffered_reader *reader);

int __must_check write_index_page_map(struct index_page_map *map,
				      struct buffered_writer *writer);

void update_index_page_map(struct index_page_map *map,
			   uint64_t virtual_chapter_number,
			   unsigned int chapter_number,
			   unsigned int index_page_number,
			   unsigned int delta_list_number);

unsigned int __must_check
find_index_page_number(const struct index_page_map *map,
		       const struct uds_chunk_name *name,
		       unsigned int chapter_number);

void get_list_number_bounds(const struct index_page_map *map,
			    unsigned int chapter_number,
			    unsigned int index_page_number,
			    unsigned int *lowest_list,
			    unsigned int *highest_list);

uint64_t compute_index_page_map_save_size(const struct geometry *geometry);

#endif /* INDEX_PAGE_MAP_H */
