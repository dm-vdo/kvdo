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
 * $Id: //eng/uds-releases/krusty-rhel9.0-beta/src/uds/chapterIndex.h#1 $
 */

#ifndef CHAPTER_INDEX_H
#define CHAPTER_INDEX_H 1

#include "deltaIndex.h"
#include "geometry.h"

enum {
	// The value returned as the record page number when an entry is not
	// found
	// in the chapter index.
	NO_CHAPTER_INDEX_ENTRY = -1
};

struct open_chapter_index {
	const struct geometry *geometry;
	struct delta_index delta_index;
	uint64_t virtual_chapter_number;
	uint64_t volume_nonce;
};


/**
 * Make a new open chapter index.
 *
 * @param open_chapter_index  Location to hold new open chapter index pointer
 * @param geometry            The geometry
 * @param volume_nonce        The volume nonce
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check
make_open_chapter_index(struct open_chapter_index **open_chapter_index,
			const struct geometry *geometry,
			uint64_t volume_nonce);

/**
 * Terminate and clean up an open chapter index.
 *
 * @param open_chapter_index  The open chapter index to terminate
 **/
void free_open_chapter_index(struct open_chapter_index *open_chapter_index);

/**
 * Empty an open chapter index, and prepare it for writing a new virtual
 * chapter.
 *
 * @param open_chapter_index      The open chapter index to empty
 * @param virtual_chapter_number  The virtual chapter number
 **/
void empty_open_chapter_index(struct open_chapter_index *open_chapter_index,
			      uint64_t virtual_chapter_number);

/**
 * Create a new record in an open chapter index, associating a chunk name with
 * the number of the record page containing the metadata for the chunk.
 *
 * @param open_chapter_index  The open chapter index
 * @param name                The chunk name
 * @param page_number         The number of the record page containing the name
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
put_open_chapter_index_record(struct open_chapter_index *open_chapter_index,
			      const struct uds_chunk_name *name,
			      unsigned int page_number);

/**
 * Pack a section of an open chapter index into a chapter index page.  A
 * range of delta lists (starting with a specified list index) is copied
 * from the open chapter index into a memory page.  The number of lists
 * copied onto the page is returned to the caller.
 *
 * @param open_chapter_index  The open chapter index
 * @param memory              The memory page to use
 * @param first_list          The first delta list number to be copied
 * @param last_page           If true, this is the last page of the chapter
 *                            index and all the remaining lists must be packed
 *                            onto this page
 * @param num_lists           The number of delta lists that were copied
 *
 * @return error code or UDS_SUCCESS.  On UDS_SUCCESS, the num_lists
 *         argument contains the number of lists copied.
 **/
int __must_check
pack_open_chapter_index_page(struct open_chapter_index *open_chapter_index,
			     byte *memory,
			     unsigned int first_list,
			     bool last_page,
			     unsigned int *num_lists);

/**
 * Get the number of records in an open chapter index.
 *
 * @param open_chapter_index  The open chapter index
 *
 * @return The number of records
 **/
int __must_check
get_open_chapter_index_size(struct open_chapter_index *open_chapter_index);

/**
 * Get the number of bytes allocated for the open chapter index.
 *
 * @param open_chapter_index  The open chapter index
 *
 * @return the number of bytes allocated
 **/
size_t
get_open_chapter_index_memory_allocated(struct open_chapter_index *open_chapter_index);

/**
 * Make a new chapter index page, initializing it with the data from the
 * given buffer.
 *
 * @param chapter_index_page  The new chapter index page
 * @param geometry            The geometry
 * @param index_page          The memory page to use
 * @param volume_nonce        If non-zero, the volume nonce to verify
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
initialize_chapter_index_page(struct delta_index_page *chapter_index_page,
			      const struct geometry *geometry,
			      byte *index_page,
			      uint64_t volume_nonce);

/**
 * Validate a chapter index page.  This is called at rebuild time to ensure
 * that the volume file contains a coherent chapter index.
 *
 * @param chapter_index_page  The chapter index page
 * @param geometry            The geometry of the volume
 *
 * @return The result code:
 *         UDS_SUCCESS for a good chapter index page
 *         UDS_CORRUPT_COMPONENT if the chapter index code detects invalid data
 *         UDS_CORRUPT_DATA if there is a problem in a delta list bit stream
 *         UDS_BAD_STATE if the code follows an invalid code path
 **/
int __must_check
validate_chapter_index_page(const struct delta_index_page *chapter_index_page,
			    const struct geometry *geometry);

/**
 * Search a chapter index page for a chunk name, returning the record page
 * number that may contain the name.
 *
 * @param [in]  chapter_index_page  The chapter index page
 * @param [in]  geometry            The geometry of the volume
 * @param [in]  name                The chunk name
 * @param [out] record_page_ptr     The record page number
 *                                  or NO_CHAPTER_INDEX_ENTRY if not found
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
search_chapter_index_page(struct delta_index_page *chapter_index_page,
			  const struct geometry *geometry,
			  const struct uds_chunk_name *name,
			  int *record_page_ptr);

#endif /* CHAPTER_INDEX_H */
