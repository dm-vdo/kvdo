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
 * $Id: //eng/uds-releases/krusty/src/uds/indexInternals.c#4 $
 */

#include "indexInternals.h"

#include "errors.h"
#include "indexCheckpoint.h"
#include "indexStateData.h"
#include "indexZone.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "openChapter.h"
#include "request.h"
#include "stringUtils.h"
#include "threads.h"
#include "typeDefs.h"
#include "volume.h"
#include "zone.h"

static const unsigned int MAX_COMPONENT_COUNT = 4;

/**********************************************************************/
int allocateIndex(struct index_layout          *layout,
                  const struct configuration   *config,
                  const struct uds_parameters  *userParams,
                  unsigned int                  zoneCount,
                  LoadType                      loadType,
                  Index                       **newIndex)
{
  unsigned int checkpoint_frequency
    = userParams == NULL ? 0 : userParams->checkpoint_frequency;
  if (checkpoint_frequency >= config->geometry->chaptersPerVolume) {
    return UDS_BAD_CHECKPOINT_FREQUENCY;
  }

  Index *index;
  int result = ALLOCATE(1, Index, "index", &index);
  if (result != UDS_SUCCESS) {
    return result;
  }

  index->existed             = (loadType != LOAD_CREATE);
  index->hasSavedOpenChapter = true;
  index->loadedType          = LOAD_UNDEFINED;

  result = makeIndexCheckpoint(index);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }
  setIndexCheckpointFrequency(index->checkpoint, checkpoint_frequency);

  get_index_layout(layout, &index->layout);
  index->zoneCount = zoneCount;

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

  result = makeVolume(config, index->layout, userParams,
                      VOLUME_CACHE_DEFAULT_MAX_QUEUED_READS, index->zoneCount,
                      &index->volume);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }
  index->volume->lookupMode  = LOOKUP_NORMAL;

  unsigned int i;
  for (i = 0; i < index->zoneCount; i++) {
    result = makeIndexZone(index, i);
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
    unsigned int i;
    for (i = 0; i < index->zoneCount; i++) {
      freeIndexZone(index->zones[i]);
    }
    FREE(index->zones);
  }

  freeVolume(index->volume);

  freeIndexState(&index->state);
  freeIndexCheckpoint(index->checkpoint);
  put_index_layout(&index->layout);
  FREE(index);
}
