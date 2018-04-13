/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/gloria/src/uds/chapterWriter.h#1 $
 */

#ifndef CHAPTER_WRITER_H
#define CHAPTER_WRITER_H

#include "atomicDefs.h"
#include "openChapterZone.h"

typedef struct chapterWriter ChapterWriter;

// This opaque declaration breaks the dependency loop with index.h
struct index;


/**
 * Create a chapter writer and start its thread.
 *
 * @param index      the index containing the chapters to be written
 * @param writerPtr  a pointer to hold the new writer
 *
 * @return           UDS_SUCCESS or an error code
 **/
int makeChapterWriter(struct index   *index,
                      ChapterWriter **writerPtr)
  __attribute__((warn_unused_result));

/**
 * Free a chapter writer, waiting for its thread to finish.
 *
 * @param writer  the chapter writer to destroy
 **/
void freeChapterWriter(ChapterWriter *writer);

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
unsigned int startClosingChapter(ChapterWriter   *writer,
                                 unsigned int     zoneNumber,
                                 OpenChapterZone *chapter)
  __attribute__((warn_unused_result));

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
int finishPreviousChapter(ChapterWriter *writer, uint64_t currentChapterNumber)
  __attribute__((warn_unused_result));

/**
 * Stop the chapter writer and wait for it to finish.
 *
 * @param writer the chapter writer to stop
 *
 * @return UDS_SUCCESS or an error code from the most recent write
 *         request
 **/
int stopChapterWriter(ChapterWriter *writer)
  __attribute__((warn_unused_result));

/**
 * Get the number of bytes allocated for the chapter writer.
 *
 * @param writer the chapter writer
 *
 * @return the number of bytes allocated
 **/
size_t getChapterWriterMemoryAllocated(ChapterWriter *writer);

#endif /* CHAPTER_WRITER_H */
