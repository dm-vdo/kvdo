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

#ifndef OPENCHAPTER_H
#define OPENCHAPTER_H 1

#include "common.h"
#include "geometry.h"
#include "index.h"
#include "index-component.h"

extern const struct index_component_info OPEN_CHAPTER_INFO;

/**
 * OpenChapter handles writing the open chapter records to the volume. It also
 * manages the open chapter index component, and all the tools to generate and
 * parse the open chapter file. The open chapter file interleaves records from
 * each openChapterZone structure.
 *
 * <p>Once each open chapter zone is filled, the records are interleaved to
 * preserve temporal locality, the index pages are generated through a
 * delta chapter index, and the record pages are derived by sorting each
 * page-sized batch of records by their names.
 *
 * <p>Upon index shutdown, the open chapter zone records are again
 * interleaved, and the records are stored as a single array. The hash
 * slots are not preserved, since the records may be reassigned to new
 * zones at load time.
 **/

/**
 * Close the open chapter and write it to disk.
 *
 * @param chapter_zones          The zones of the chapter to close
 * @param zone_count             The number of zones
 * @param volume                 The volume to which to write the chapter
 * @param chapter_index          The open_chapter_index to use while writing
 * @param collated_records       Collated records array to use while writing
 * @param virtual_chapter_number The virtual chapter number of the open chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check close_open_chapter(struct open_chapter_zone **chapter_zones,
				    unsigned int zone_count,
				    struct volume *volume,
				    struct open_chapter_index *chapter_index,
				    struct uds_chunk_record *collated_records,
				    uint64_t virtual_chapter_number);

/**
 * Write out a partially filled chapter to a file.
 *
 * @param index        the index to save the data from
 * @param writer       the writer to write out the chapters
 *
 * @return UDS_SUCCESS on success
 **/
int __must_check save_open_chapters(struct uds_index *index,
				    struct buffered_writer *writer);

/**
 * Read a partially filled chapter from a file.
 *
 * @param index        the index to load the data into
 * @param reader       the buffered reader to read from
 *
 * @return UDS_SUCCESS on success
 **/
int __must_check load_open_chapters(struct uds_index *index,
				    struct buffered_reader *reader);

/**
 * Compute the size of the maximum open chapter save image.
 *
 * @param geometry      the index geometry
 *
 * @return the number of bytes of the largest possible open chapter save
 *         image
 **/
uint64_t compute_saved_open_chapter_size(struct geometry *geometry);

#endif /* OPENCHAPTER_H */
