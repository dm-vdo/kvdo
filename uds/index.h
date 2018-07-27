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
 * $Id: //eng/uds-releases/gloria/src/uds/index.h#4 $
 */

#ifndef INDEX_H
#define INDEX_H

#include "chapterWriter.h"
#include "indexRouterStats.h"
#include "indexLayout.h"
#include "indexZone.h"
#include "loadType.h"
#include "masterIndexOps.h"
#include "volume.h"

/**
 * Index checkpoint state private to indexCheckpoint.c.
 **/
typedef struct indexCheckpoint IndexCheckpoint;

typedef struct index {
  bool           existed;
  IndexLayout   *layout;
  IndexState    *state;
  MasterIndex   *masterIndex;
  Volume        *volume;
  unsigned int   zoneCount;
  IndexZone    **zones;

  /*
   * ATTENTION!!!
   * The meaning of the next two fields has changed.
   *
   * They now represent the oldest and newest chapters only at load time,
   * and when the index is quiescent. At other times, they may lag individual
   * zones' views of the index depending upon the progress made by the chapter
   * writer.
   */
  uint64_t       oldestVirtualChapter;
  uint64_t       newestVirtualChapter;

  uint64_t       lastCheckpoint;
  uint64_t       prevCheckpoint;
  ChapterWriter *chapterWriter;

  // checkpoint state used by indexCheckpoint.c
  IndexCheckpoint *checkpoint;
} Index;

/**
 * Construct a new index from the given configuration.
 *
 * @param layout         The index layout
 * @param config         The configuration to use
 * @param zoneCount      The number of zones for this index to use
 * @param loadType       How to create the index:  it can be create only,
 *                       allow loading from files, and allow rebuilding
 *                       from the volume
 * @param newIndex       A pointer to hold a pointer to the new index
 *
 * @return         UDS_SUCCESS or an error code
 **/
int makeIndex(IndexLayout          *layout,
              const Configuration  *config,
              unsigned int          zoneCount,
              LoadType              loadType,
              Index               **newIndex)
  __attribute__((warn_unused_result));

/**
 * Save an index.
 *
 * After this operation completes, the index must be freed.  Saving the index
 * will shutdown the chapter writer, and there is no provision for restarting
 * it.
 *
 * The normal users follow saveIndex immediately with a freeIndex.  But some
 * tests use the IndexLayout to modify the saved index.  The Index will then
 * have some cached information that does not reflect these updates.
 *
 * XXX - If we put a use count on the IndexLayout, and implement a get/put
 *       mechanism, we can refactor into a safer saveAndFreeIndex method.  The
 *       tests could then "get" the IndexLayout and use it to modify the saved
 *       index after the Index is freed.
 *
 * @param index   The index to save
 *
 * @return        UDS_SUCCESS if successful
 **/
int saveIndex(Index *index) __attribute__((warn_unused_result));

/**
 * Clean up the index and its memory.
 *
 * @param index   The index to destroy.
 **/
void freeIndex(Index *index);

/**
 * Perform the index operation specified by the action field of a UDS request.
 *
 * For UDS API requests, this searches the index for the chunk name in the
 * request. If the chunk name is already present in the index, the location
 * field of the request will be set to the IndexRegion where it was found. If
 * the action is not DELETE, the oldMetadata field of the request will also be
 * filled in with the prior metadata for the name.
 *
 * If the API request action is:
 *
 *   REQUEST_INDEX, a record will be added to the open chapter with the
 *     metadata in the request for new records, and the existing metadata for
 *     existing records
 *
 *   REQUEST_UPDATE, a record will be added to the open chapter with the
 *     metadata in the request
 *
 *   REQUEST_QUERY, if the update flag is set in the request, any record
 *     found will be moved to the open chapter. In all other cases the contents
 *     of the index will remain unchanged.
 *
 *   REQUEST_REMOVE, the any entry with the name will removed from the index
 *
 * For non-API requests, no chunk name search is involved.
 *
 * @param index       The index
 * @param request     The originating request
 *
 * @return UDS_SUCCESS, UDS_QUEUED, or an error code
 **/
int dispatchIndexRequest(Index *index, Request *request)
  __attribute__((warn_unused_result));

/**
 * Internal helper to prepare the index for saving.
 *
 * @param index              the index
 * @param checkpoint         whether the save is a checkpoint
 * @param openChapterNumber  the virtual chapter number of the open chapter
 **/
void beginSave(Index *index, bool checkpoint, uint64_t openChapterNumber);

/**
 * Replay the volume file to repopulate the master index.
 *
 * @param index         The index
 * @param fromVCN       The virtual chapter to start replaying
 *
 * @return              UDS_SUCCESS if successful
 **/
int replayVolume(Index *index, uint64_t fromVCN)
  __attribute__((warn_unused_result));

/**
 * Gather statistics from the master index, volume, and cache.
 *
 * @param index     The index
 * @param counters  the statistic counters for the index
 **/
void getIndexStats(Index *index, IndexRouterStatCounters *counters);

/**
 * Set lookup state for this index.  Disabling lookups means assume
 * all records queried are new (intended for debugging uses, e.g.,
 * albfill).
 *
 * @param index     The index
 * @param enabled   The new lookup state
 **/
void setIndexLookupState(Index *index, bool enabled);

/**
 * Advance the newest virtual chapter. If this will overwrite the oldest
 * virtual chapter, advance that also.
 *
 * @param index The index to advance
 **/
void advanceActiveChapters(Index *index);

/**
 * Triage an index request, deciding whether it requires that a sparse cache
 * barrier message precede it.
 *
 * This resolves the chunk name in the request in the master index,
 * determining if it is a hook or not, and if a hook, what virtual chapter (if
 * any) it might be found in. If a virtual chapter is found, it checks whether
 * that chapter appears in the sparse region of the index. If all these
 * conditions are met, the (sparse) virtual chapter number is returned. In all
 * other cases it returns <code>UINT64_MAX</code>.
 *
 * @param index    the index that will process the request
 * @param request  the index request containing the chunk name to triage
 *
 * @return the sparse chapter number for the sparse cache barrier message, or
 *         <code>UINT64_MAX</code> if the request does not require a barrier
 **/
uint64_t triageIndexRequest(Index *index, Request *request)
  __attribute__((warn_unused_result));

#endif /* INDEX_H */
