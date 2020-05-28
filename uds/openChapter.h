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
 * $Id: //eng/uds-releases/krusty/src/uds/openChapter.h#8 $
 */

#ifndef OPENCHAPTER_H
#define OPENCHAPTER_H 1

#include "common.h"
#include "geometry.h"
#include "index.h"
#include "indexComponent.h"

extern const IndexComponentInfo OPEN_CHAPTER_INFO;

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
 * @param chapterZones         The zones of the chapter to close
 * @param zoneCount            The number of zones
 * @param volume               The volume to which to write the chapter
 * @param chapterIndex         The open_chapter_index to use while writing
 * @param collatedRecords      Collated records array to use while writing
 * @param virtualChapterNumber The virtual chapter number of the open chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check closeOpenChapter(OpenChapterZone **chapterZones,
				  unsigned int zoneCount,
				  Volume *volume,
				  struct open_chapter_index *chapterIndex,
				  struct uds_chunk_record *collatedRecords,
				  uint64_t virtualChapterNumber);

/**
 * Write out a partially filled chapter to a file.
 *
 * @param index        the index to save the data from
 * @param writer       the writer to write out the chapters
 *
 * @return UDS_SUCCESS on success
 **/
int __must_check saveOpenChapters(struct index *index,
				  struct buffered_writer *writer);

/**
 * Read a partially filled chapter from a file.
 *
 * @param index        the index to load the data into
 * @param reader       the buffered reader to read from
 *
 * @return UDS_SUCCESS on success
 **/
int __must_check loadOpenChapters(struct index *index,
				  struct buffered_reader *reader);

/**
 * Compute the size of the maximum open chapter save image.
 *
 * @param geometry      the index geometry
 *
 * @return the number of bytes of the largest possible open chapter save
 *         image
 **/
uint64_t computeSavedOpenChapterSize(struct geometry *geometry);

#endif /* OPENCHAPTER_H */
