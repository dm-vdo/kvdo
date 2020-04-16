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
 * $Id: //eng/uds-releases/krusty/src/uds/chapterWriter.h#2 $
 */

#ifndef CHAPTER_WRITER_H
#define CHAPTER_WRITER_H

#include "atomicDefs.h"
#include "indexVersion.h"
#include "openChapterZone.h"

struct chapter_writer;

// This opaque declaration breaks the dependency loop with index.h
struct index;


/**
 * Create a chapter writer and start its thread.
 *
 * @param index         the index containing the chapters to be written
 * @param indexVersion  the index version parameters
 * @param writerPtr     pointer to hold the new writer
 *
 * @return           UDS_SUCCESS or an error code
 **/
int __must_check makeChapterWriter(struct index *index,
				   const struct index_version *indexVersion,
				   struct chapter_writer **writerPtr);

/**
 * Free a chapter writer, waiting for its thread to finish.
 *
 * @param writer  the chapter writer to destroy
 **/
void freeChapterWriter(struct chapter_writer *writer);

/**
 * Asychronously close and write a chapter by passing it to the writer
 * thread. Writing won't start until all zones have submitted a chapter.
 *
 * @param writer     the chapter writer
 * @param zoneNumber the number of the zone submitting a chapter
 * @param chapter    the chapter to write
 *
 * @return The number of zones which have submitted the current chapter
 **/
unsigned int __must_check
startClosingChapter(struct chapter_writer *writer,
		    unsigned int zoneNumber,
		    OpenChapterZone *chapter);

/**
 * Wait for the chapter writer thread to finish closing the chapter previous
 * to the one specified.
 *
 * @param writer               the chapter writer
 * @param currentChapterNumber the currentChapter number
 *
 * @return UDS_SUCCESS or an error code from the most recent write
 *         request
 **/
int __must_check
finishPreviousChapter(struct chapter_writer *writer,
		      uint64_t currentChapterNumber);

/**
 * Wait for the chapter writer thread to finish all writes to storage.
 *
 * @param writer  the chapter writer
 **/
void waitForIdleChapterWriter(struct chapter_writer *writer);

/**
 * Stop the chapter writer and wait for it to finish.
 *
 * @param writer  the chapter writer to stop
 *
 * @return UDS_SUCCESS or an error code from the most recent write
 *         request
 **/
int __must_check stopChapterWriter(struct chapter_writer *writer);

/**
 * Get the number of bytes allocated for the chapter writer.
 *
 * @param writer the chapter writer
 *
 * @return the number of bytes allocated
 **/
size_t getChapterWriterMemoryAllocated(struct chapter_writer *writer);

#endif /* CHAPTER_WRITER_H */
