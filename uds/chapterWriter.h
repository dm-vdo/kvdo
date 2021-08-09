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
 * $Id: //eng/uds-releases/lisa/src/uds/chapterWriter.h#1 $
 */

#ifndef CHAPTER_WRITER_H
#define CHAPTER_WRITER_H

#include <linux/atomic.h>

#include "openChapterZone.h"

struct chapter_writer;

// This opaque declaration breaks the dependency loop with index.h
struct uds_index;


/**
 * Create a chapter writer and start its thread.
 *
 * @param index       the index containing the chapters to be written
 * @param writer_ptr  pointer to hold the new writer
 *
 * @return           UDS_SUCCESS or an error code
 **/
int __must_check make_chapter_writer(struct uds_index *index,
				     struct chapter_writer **writer_ptr);

/**
 * Free a chapter writer, waiting for its thread to finish.
 *
 * @param writer  the chapter writer to destroy
 **/
void free_chapter_writer(struct chapter_writer *writer);

/**
 * Asychronously close and write a chapter by passing it to the writer
 * thread. Writing won't start until all zones have submitted a chapter.
 *
 * @param writer       the chapter writer
 * @param zone_number  the number of the zone submitting a chapter
 * @param chapter      the chapter to write
 *
 * @return The number of zones which have submitted the current chapter
 **/
unsigned int __must_check
start_closing_chapter(struct chapter_writer *writer,
		      unsigned int zone_number,
		      struct open_chapter_zone *chapter);

/**
 * Wait for the chapter writer thread to finish closing the chapter previous
 * to the one specified.
 *
 * @param writer                  the chapter writer
 * @param current_chapter_number  the current chapter number
 *
 * @return UDS_SUCCESS or an error code from the most recent write
 *         request
 **/
int __must_check finish_previous_chapter(struct chapter_writer *writer,
					 uint64_t current_chapter_number);

/**
 * Wait for the chapter writer thread to finish all writes to storage.
 *
 * @param writer  the chapter writer
 **/
void wait_for_idle_chapter_writer(struct chapter_writer *writer);

/**
 * Stop the chapter writer and wait for it to finish.
 *
 * @param writer  the chapter writer to stop
 *
 * @return UDS_SUCCESS or an error code from the most recent write
 *         request
 **/
int __must_check stop_chapter_writer(struct chapter_writer *writer);

/**
 * Get the number of bytes allocated for the chapter writer.
 *
 * @param writer the chapter writer
 *
 * @return the number of bytes allocated
 **/
size_t get_chapter_writer_memory_allocated(struct chapter_writer *writer);

#endif /* CHAPTER_WRITER_H */
