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
 * $Id: //eng/uds-releases/krusty/src/uds/chapterWriter.c#13 $
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


struct chapter_writer {
	/* The index to which we belong */
	struct index *index;
	/* The thread to do the writing */
	Thread thread;
	/* lock protecting the following fields */
	Mutex mutex;
	/* condition signalled on state changes */
	CondVar cond;
	/* Set to true to stop the thread */
	bool stop;
	/* The result from the most recent write */
	int result;
	/* The number of bytes allocated by the chapter writer */
	size_t memory_allocated;
	/* The number of zones which have submitted a chapter for writing */
	unsigned int zones_to_write;
	/* Open chapter index used by closeOpenChapter() */
	struct open_chapter_index *open_chapter_index;
	/* Collated records used by closeOpenChapter() */
	struct uds_chunk_record *collated_records;
	/* The chapters to write (one per zone) */
	OpenChapterZone *chapters[];
};

/**
 * This is the driver function for the writer thread. It loops until
 * terminated, waiting for a chapter to provided to close.
 **/
static void close_chapters(void *arg)
{
	struct chapter_writer *writer = arg;
	logDebug("chapter writer starting");
	lockMutex(&writer->mutex);
	for (;;) {
		while (writer->zones_to_write < writer->index->zone_count) {
			if (writer->stop && (writer->zones_to_write == 0)) {
				// We've been told to stop, and all of the
				// zones are in the same open chapter, so we
				// can exit now.
				unlockMutex(&writer->mutex);
				logDebug("chapter writer stopping");
				return;
			}
			waitCond(&writer->cond, &writer->mutex);
		}

		/*
		 * Release the lock while closing a chapter. We probably don't
		 * need to do this, but it seems safer in principle. It's OK to
		 * access the chapter and chapterNumber fields without the lock
		 * since those aren't allowed to change until we're done.
		 */
		unlockMutex(&writer->mutex);

		if (writer->index->has_saved_open_chapter) {
			writer->index->has_saved_open_chapter = false;
			/*
			 * Remove the saved open chapter as that chapter is
			 * about to be written to the volume.  This matters the
			 * first time we close the open chapter after loading
			 * from a clean shutdown, or after doing a clean save.
			 */
			struct index_component *oc =
				find_index_component(writer->index->state,
			                             &OPEN_CHAPTER_INFO);
			int result = discard_index_component(oc);
			if (result == UDS_SUCCESS) {
				logDebug("Discarding saved open chapter");
			}
		}

		int result =
			closeOpenChapter(writer->chapters,
					 writer->index->zone_count,
					 writer->index->volume,
					 writer->open_chapter_index,
					 writer->collated_records,
					 writer->index->newest_virtual_chapter);

		if (result == UDS_SUCCESS) {
			result = process_chapter_writer_checkpoint_saves(writer->index);
		}


		lockMutex(&writer->mutex);
		// Note that the index is totally finished with the writing
		// chapter
		advance_active_chapters(writer->index);
		writer->result = result;
		writer->zones_to_write = 0;
		broadcastCond(&writer->cond);
	}
}

/**********************************************************************/
int make_chapter_writer(struct index *index,
			const struct index_version *index_version,
			struct chapter_writer **writer_ptr)
{
	size_t collated_records_size =
		(sizeof(struct uds_chunk_record) *
		 (1 + index->volume->geometry->records_per_chapter));
	struct chapter_writer *writer;
	int result = ALLOCATE_EXTENDED(struct chapter_writer,
				       index->zone_count,
				       OpenChapterZone *,
				       "Chapter Writer",
				       &writer);
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

	// Now that we have the mutex+cond, it is safe to call
	// free_chapter_writer.
	result = allocateCacheAligned(collated_records_size,
				      "collated records",
				      &writer->collated_records);
	if (result != UDS_SUCCESS) {
		free_chapter_writer(writer);
		return makeUnrecoverable(result);
	}
	result = make_open_chapter_index(
		&writer->open_chapter_index,
		index->volume->geometry,
		index_version->chapterIndexHeaderNativeEndian,
		index->volume->nonce);
	if (result != UDS_SUCCESS) {
		free_chapter_writer(writer);
		return makeUnrecoverable(result);
	}

	size_t open_chapter_index_memory_allocated =
		get_open_chapter_index_memory_allocated(
			writer->open_chapter_index);
	writer->memory_allocated =
		(sizeof(struct chapter_writer) +
		 index->zone_count * sizeof(OpenChapterZone *) +
		 collated_records_size + open_chapter_index_memory_allocated);

	// We're initialized, so now it's safe to start the writer thread.
	result = createThread(close_chapters, writer, "writer", &writer->thread);
	if (result != UDS_SUCCESS) {
		free_chapter_writer(writer);
		return makeUnrecoverable(result);
	}

	*writer_ptr = writer;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_chapter_writer(struct chapter_writer *writer)
{
	if (writer == NULL) {
		return;
	}

	int result __attribute__((unused)) = stop_chapter_writer(writer);
	destroyMutex(&writer->mutex);
	destroyCond(&writer->cond);
	free_open_chapter_index(writer->open_chapter_index);
	FREE(writer->collated_records);
	FREE(writer);
}

/**********************************************************************/
unsigned int start_closing_chapter(struct chapter_writer *writer,
				   unsigned int zone_number,
				   OpenChapterZone *chapter)
{
	lockMutex(&writer->mutex);
	unsigned int finished_zones = ++writer->zones_to_write;
	writer->chapters[zone_number] = chapter;
	broadcastCond(&writer->cond);
	unlockMutex(&writer->mutex);

	return finished_zones;
}

/**********************************************************************/
int finish_previous_chapter(struct chapter_writer *writer,
			    uint64_t current_chapter_number)
{
	int result;
	lockMutex(&writer->mutex);
	while (writer->index->newest_virtual_chapter <
	       current_chapter_number) {
		waitCond(&writer->cond, &writer->mutex);
	}
	result = writer->result;
	unlockMutex(&writer->mutex);

	if (result != UDS_SUCCESS) {
		return logUnrecoverable(
			result, "Writing of previous open chapter failed");
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
void wait_for_idle_chapter_writer(struct chapter_writer *writer)
{
	lockMutex(&writer->mutex);
	while (writer->zones_to_write > 0) {
		// The chapter writer is probably writing a chapter.  If it is
		// not, it will soon wake up and write a chapter.
		waitCond(&writer->cond, &writer->mutex);
	}
	unlockMutex(&writer->mutex);
}

/**********************************************************************/
int stop_chapter_writer(struct chapter_writer *writer)
{
	Thread writer_thread = 0;

	lockMutex(&writer->mutex);
	if (writer->thread != 0) {
		writer_thread = writer->thread;
		writer->thread = 0;
		writer->stop = true;
		broadcastCond(&writer->cond);
	}
	int result = writer->result;
	unlockMutex(&writer->mutex);

	if (writer_thread != 0) {
		joinThreads(writer_thread);
	}

	if (result != UDS_SUCCESS) {
		return logUnrecoverable(
			result, "Writing of previous open chapter failed");
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
size_t get_chapter_writer_memory_allocated(struct chapter_writer *writer)
{
	return writer->memory_allocated;
}
