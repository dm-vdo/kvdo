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
 * $Id: //eng/uds-releases/gloria/src/uds/chapterWriter.c#3 $
 */

#include "chapterWriter.h"

#include "errors.h"
#include "index.h"
#include "indexCheckpoint.h"
#include "indexComponent.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "openChapter.h"
#include "threads.h"


struct chapterWriter {
  /* The index to which we belong */
  Index            *index;
  /* The thread to do the writing */
  Thread            thread;
  /* lock protecting the following fields */
  Mutex             mutex;
  /* condition signalled on state changes */
  CondVar           cond;
  /* Set to true to stop the thread */
  bool              stop;
  /* The result from the most recent write */
  int               result;
  /* The number of bytes allocated by the chapter writer */
  size_t            memoryAllocated;
  /* The number of zones which have submitted a chapter for writing */
  unsigned int      zonesToWrite;
  /* Open chapter index used by closeOpenChapter() */
  OpenChapterIndex *openChapterIndex;
  /* Collated records used by closeOpenChapter() */
  UdsChunkRecord   *collatedRecords;
  /* The chapters to write (one per zone) */
  OpenChapterZone  *chapters[];
};

/**
 * This is the driver function for the writer thread. It loops until
 * terminated, waiting for a chapter to provided to close.
 **/
static void closeChapters(void *arg)
{
  ChapterWriter *writer = arg;
  logDebug("chapter writer starting");
  lockMutex(&writer->mutex);
  bool firstChapterWrite = true;
  for (;;) {
    while (writer->zonesToWrite < writer->index->zoneCount) {
      if (writer->stop && (writer->zonesToWrite == 0)) {
        // We've been told to stop, and all of the zones are in the same
        // open chapter, so we can exit now.
        unlockMutex(&writer->mutex);
        logDebug("chapter writer stopping");
        return;
      }
      waitCond(&writer->cond, &writer->mutex);
    }

    /*
     * Release the lock while closing a chapter. We probably don't need to do
     * this, but it seems safer in principle. It's OK to access the chapter
     * and chapterNumber fields without the lock since those aren't allowed to
     * change until we're done.
     */
    unlockMutex(&writer->mutex);

    if (firstChapterWrite) {
      firstChapterWrite = false;
      /*
       * Remove the old open chapter file as that chapter is about to be
       * moved to the volume. This only matters the first time we close
       * the open chapter after loading from a clean shutdown.
       */
      IndexComponent *oc = findIndexComponent(writer->index->state,
                                              &OPEN_CHAPTER_INFO);
      int result = discardIndexComponent(oc);
      if (result == UDS_SUCCESS) {
        logDebug("Discarding saved open chapter");
      }
    }

    int result = closeOpenChapter(writer->chapters,
                                  writer->index->zoneCount,
                                  writer->index->volume,
                                  writer->openChapterIndex,
                                  writer->collatedRecords,
                                  writer->index->newestVirtualChapter);

    if (result == UDS_SUCCESS) {
      result = processChapterWriterCheckpointSaves(writer->index);
    }

    lockMutex(&writer->mutex);
    // Note that the index is totally finished with the writing chapter
    advanceActiveChapters(writer->index);
    writer->result       = result;
    writer->zonesToWrite = 0;
    broadcastCond(&writer->cond);
  }
}

/**********************************************************************/
int makeChapterWriter(Index *index, ChapterWriter **writerPtr)
{
  size_t collatedRecordsSize
    = (sizeof(UdsChunkRecord)
       * (1 + index->volume->geometry->recordsPerChapter));
  ChapterWriter *writer;
  int result = ALLOCATE_EXTENDED(ChapterWriter,
                                 index->zoneCount, OpenChapterZone *,
                                 "Chapter Writer", &writer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  writer->index = index;

  result = initMutex(&writer->mutex);
  if (result != UDS_SUCCESS) {
    FREE(writer);
    return result;
  }
  result = initCond(&writer->cond);
  if (result != UDS_SUCCESS) {
    destroyMutex(&writer->mutex);
    FREE(writer);
    return result;
  }

  // Now that we have the mutex+cond, it is safe to call freeChapterWriter.
  result = allocateCacheAligned(collatedRecordsSize, "collated records",
                                &writer->collatedRecords);
  if (result != UDS_SUCCESS) {
    freeChapterWriter(writer);
    return makeUnrecoverable(result);
  }
  result = makeOpenChapterIndex(&writer->openChapterIndex,
                                index->volume->geometry);
  if (result != UDS_SUCCESS) {
    freeChapterWriter(writer);
    return makeUnrecoverable(result);
  }

  size_t openChapterIndexMemoryAllocated
    = getOpenChapterIndexMemoryAllocated(writer->openChapterIndex);
  writer->memoryAllocated = (sizeof(ChapterWriter)
                             + index->zoneCount * sizeof(OpenChapterZone *)
                             + collatedRecordsSize
                             + openChapterIndexMemoryAllocated);

  // We're initialized, so now it's safe to start the writer thread.
  result = createThread(closeChapters, writer, "writer", &writer->thread);
  if (result != UDS_SUCCESS) {
    freeChapterWriter(writer);
    return makeUnrecoverable(result);
  }

  *writerPtr = writer;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeChapterWriter(ChapterWriter *writer)
{
  if (writer == NULL) {
    return;
  }

  int result __attribute__((unused)) = stopChapterWriter(writer);
  destroyMutex(&writer->mutex);
  destroyCond(&writer->cond);
  freeOpenChapterIndex(writer->openChapterIndex);
  FREE(writer->collatedRecords);
  FREE(writer);
}

/**********************************************************************/
unsigned int startClosingChapter(ChapterWriter   *writer,
                                 unsigned int     zoneNumber,
                                 OpenChapterZone *chapter)
{
  lockMutex(&writer->mutex);
  unsigned int finishedZones = ++writer->zonesToWrite;
  writer->chapters[zoneNumber] = chapter;
  broadcastCond(&writer->cond);
  unlockMutex(&writer->mutex);

  return finishedZones;
}

/**********************************************************************/
int finishPreviousChapter(ChapterWriter *writer, uint64_t currentChapterNumber)
{
  int result;
  lockMutex(&writer->mutex);
  while (writer->index->newestVirtualChapter < currentChapterNumber) {
    waitCond(&writer->cond, &writer->mutex);
  }
  result = writer->result;
  unlockMutex(&writer->mutex);

  if (result != UDS_SUCCESS) {
    return logUnrecoverable(result, "Writing of previous open chapter failed");
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int stopChapterWriter(ChapterWriter *writer)
{
  Thread writerThread = 0;

  lockMutex(&writer->mutex);
  if (writer->thread != 0) {
    writerThread = writer->thread;
    writer->thread = 0;
    writer->stop = true;
    broadcastCond(&writer->cond);
  }
  int result = writer->result;
  unlockMutex(&writer->mutex);

  if (writerThread != 0) {
    joinThreads(writerThread);
  }

  if (result != UDS_SUCCESS) {
    return logUnrecoverable(result, "Writing of previous open chapter failed");
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
size_t getChapterWriterMemoryAllocated(ChapterWriter *writer)
{
  return writer->memoryAllocated;
}
