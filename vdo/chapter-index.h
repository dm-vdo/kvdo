/* SPDX-License-Identifier: GPL-2.0-only */
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

#ifndef CHAPTER_INDEX_H
#define CHAPTER_INDEX_H 1

#include "delta-index.h"
#include "geometry.h"

enum {
	/*
	 * The value returned as the record page number when an entry is not
	 * found in the chapter index.
	 */
	NO_CHAPTER_INDEX_ENTRY = -1
};

struct open_chapter_index {
	const struct geometry *geometry;
	struct delta_index delta_index;
	uint64_t virtual_chapter_number;
	uint64_t volume_nonce;
	size_t memory_allocated;
};

int __must_check
make_open_chapter_index(struct open_chapter_index **chapter_index,
			const struct geometry *geometry,
			uint64_t volume_nonce);

void free_open_chapter_index(struct open_chapter_index *chapter_index);

void empty_open_chapter_index(struct open_chapter_index *chapter_index,
			      uint64_t virtual_chapter_number);

int __must_check
put_open_chapter_index_record(struct open_chapter_index *chapter_index,
			      const struct uds_chunk_name *name,
			      unsigned int page_number);

int __must_check
pack_open_chapter_index_page(struct open_chapter_index *chapter_index,
			     byte *memory,
			     unsigned int first_list,
			     bool last_page,
			     unsigned int *num_lists);

int __must_check
get_open_chapter_index_size(struct open_chapter_index *chapter_index);

int __must_check
initialize_chapter_index_page(struct delta_index_page *index_page,
			      const struct geometry *geometry,
			      byte *page_buffer,
			      uint64_t volume_nonce);

int __must_check
validate_chapter_index_page(const struct delta_index_page *index_page,
			    const struct geometry *geometry);

int __must_check
search_chapter_index_page(struct delta_index_page *index_page,
			  const struct geometry *geometry,
			  const struct uds_chunk_name *name,
			  int *record_page_ptr);

#endif /* CHAPTER_INDEX_H */
