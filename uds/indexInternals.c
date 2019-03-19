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
 * $Id: //eng/uds-releases/gloria/src/uds/indexInternals.c#5 $
 */

#include "indexInternals.h"

#include "errors.h"
#include "indexCheckpoint.h"
#include "indexStateData.h"
#include "indexZone.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "openChapter.h"
#include "readOnlyVolume.h"
#include "request.h"
#include "stringUtils.h"
#include "threads.h"
#include "typeDefs.h"
#include "volume.h"
#include "zone.h"

const bool READ_ONLY_INDEX = true;

static const unsigned int MAX_COMPONENT_COUNT = 4;

/**********************************************************************/
int allocateIndex(IndexLayout          *layout,
                  const Configuration  *config,
                  unsigned int          zoneCount,
                  LoadType              loadType,
                  bool                  readOnly,
                  Index               **newIndex)
{
  if (loadType == LOAD_CREATE) {
    if (readOnly) {
      logError("Can't create a read only index");
      return EINVAL;
    }
    IORegion *region = NULL;
    int result = openVolumeRegion(layout, IO_CREATE_WRITE, &region);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = formatVolume(region, config->geometry);
    int closeResult = closeIORegion(&region);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (closeResult != UDS_SUCCESS) {
      return closeResult;
    }
  }

  Index *index;
  int result = ALLOCATE(1, Index, "index", &index);
  if (result != UDS_SUCCESS) {
    return result;
  }

  index->existed = (loadType != LOAD_CREATE);

  result = makeIndexCheckpoint(index);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }
  setIndexCheckpointFrequency(index->checkpoint, config->checkpointFrequency);

  index->layout    = layout;
  index->zoneCount = (readOnly ? 1 : zoneCount);

  result = ALLOCATE(index->zoneCount, IndexZone *, "zones",
                    &index->zones);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }

  result = makeIndexState(layout, index->zoneCount, MAX_COMPONENT_COUNT,
                          &index->state);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }

  result = addIndexStateComponent(index->state, &INDEX_STATE_INFO, index,
                                  NULL);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }

  if (readOnly) {
    result = makeReadOnlyVolume(config, index->layout, &index->volume);
  } else {
    result = makeVolume(config, index->layout,
                        VOLUME_CACHE_DEFAULT_MAX_QUEUED_READS,
                        index->zoneCount, &index->volume);
  }

  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }
  index->volume->lookupMode  = LOOKUP_NORMAL;

  for (unsigned int i = 0; i < index->zoneCount; i++) {
    result = makeIndexZone(index, i, readOnly);
    if (result != UDS_SUCCESS) {
      freeIndex(index);
      return logErrorWithStringError(result, "Could not create index zone");
    }
  }

  result = addIndexStateComponent(index->state, &OPEN_CHAPTER_INFO, index,
                                  NULL);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return logErrorWithStringError(result, "Could not create open chapter");
  }

  *newIndex = index;
  return UDS_SUCCESS;
}

/**********************************************************************/
void releaseIndex(Index *index)
{
  if (index == NULL) {
    return;
  }

  if (index->zones != NULL) {
    for (unsigned int i = 0; i < index->zoneCount; i++) {
      freeIndexZone(index->zones[i]);
    }
    FREE(index->zones);
  }

  freeVolume(index->volume);

  freeIndexState(&index->state);
  freeIndexCheckpoint(index->checkpoint);
  FREE(index);
}
