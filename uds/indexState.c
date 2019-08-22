/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/src/uds/indexState.c#5 $
 */

#include "indexState.h"

#include "errors.h"
#include "indexComponent.h"
#include "indexLayout.h"
#include "logger.h"
#include "memoryAlloc.h"

/*****************************************************************************/
int makeIndexState(IndexLayout   *layout,
                   unsigned int   numZones,
                   unsigned int   maxComponents,
                   IndexState   **statePtr)
{
  if (maxComponents == 0) {
    return logErrorWithStringError(
      UDS_INVALID_ARGUMENT, "cannot make index state with maxComponents 0");
  }

  IndexState *state = NULL;
  int result = ALLOCATE_EXTENDED(IndexState, maxComponents, IndexComponent *,
                                 "index state", &state);
  if (result != UDS_SUCCESS) {
    return result;
  }

  state->count     = 0;
  state->layout    = layout;
  state->length    = maxComponents;
  state->loadZones = 0;
  state->loadSlot  = UINT_MAX;
  state->saveSlot  = UINT_MAX;
  state->saving    = false;
  state->zoneCount = numZones;

  *statePtr = state;
  return UDS_SUCCESS;
}

/*****************************************************************************/
void freeIndexState(IndexState **statePtr)
{
  IndexState *state = *statePtr;
  *statePtr = NULL;
  if (state != NULL) {
    unsigned int i;
    for (i = 0; i < state->count; ++i) {
      freeIndexComponent(&state->entries[i]);
    }
    FREE(state);
  }
}

/*****************************************************************************/
/**
 * Add a component to the index state.
 *
 * @param state         The index state.
 * @param component     The index component.
 *
 * @return      UDS_SUCCESS or an error code.
 **/
static int addComponentToIndexState(IndexState     *state,
                                    IndexComponent *component)
{
  if (findIndexComponent(state, component->info) != NULL) {
    return logErrorWithStringError(
      UDS_INVALID_ARGUMENT, "cannot add state component %s: already present",
      component->info->name);
  }

  if (state->count >= state->length) {
    return logErrorWithStringError(
      UDS_RESOURCE_LIMIT_EXCEEDED,
      "cannot add state component %s, %u components already added",
      component->info->name, state->count);
  }

  state->entries[state->count] = component;
  ++state->count;
  return UDS_SUCCESS;
}

/*****************************************************************************/
int addIndexStateComponent(IndexState               *state,
                           const IndexComponentInfo *info,
                           void                     *data,
                           void                     *context)
{
  IndexComponent *component = NULL;
  int result = makeIndexComponent(state, info, state->zoneCount, data, context,
                                  &component);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "cannot make region index component");
  }

  result = addComponentToIndexState(state, component);
  if (result != UDS_SUCCESS) {
    freeIndexComponent(&component);
    return result;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
IndexComponent *findIndexComponent(const IndexState         *state,
                                   const IndexComponentInfo *info)
{
  unsigned int i;
  for (i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if (info == component->info) {
      return component;
    }
  }
  return NULL;
}

/*****************************************************************************/
static const char *indexSaveTypeName(IndexSaveType saveType)
{
  return saveType == IS_SAVE ? "save" : "checkpoint";
}

/*****************************************************************************/
int loadIndexState(IndexState *state, bool *replayPtr)
{
  int result = findLatestIndexSaveSlot(state->layout, &state->loadZones,
                                       &state->loadSlot);
  if (result != UDS_SUCCESS) {
    return result;
  }

  bool replayRequired = false;
  unsigned int i;
  for (i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    result = readIndexComponent(component);
    if (result != UDS_SUCCESS) {
      if (!missingIndexComponentRequiresReplay(component)) {
        state->loadZones = 0;
        state->loadSlot  = UINT_MAX;
        return logErrorWithStringError(result, "index component %s",
                                       indexComponentName(component));
      }
      replayRequired = true;
    }
  }

  state->loadZones = 0;
  state->loadSlot  = UINT_MAX;
  if (replayPtr != NULL) {
    *replayPtr = replayRequired;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int prepareToSaveIndexState(IndexState *state, IndexSaveType saveType)
{
  if (state->saving) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "already saving the index state");
  }
  int result = setupIndexSaveSlot(state->layout, state->zoneCount, saveType,
                                  &state->saveSlot);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "cannot prepare index %s",
                                   indexSaveTypeName(saveType));
  }

  return UDS_SUCCESS;
}

/*****************************************************************************/
/**
 *  Complete the saving of an index state.
 *
 *  @param state  the index state
 *
 *  @return UDS_SUCCESS or an error code
 **/
static int completeIndexSaving(IndexState *state)
{
  state->saving = false;
  int result = commitIndexSave(state->layout, state->saveSlot);
  state->saveSlot = UINT_MAX;
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "cannot commit index state");
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int cleanupSave(IndexState *state)
{
  int result = cancelIndexSave(state->layout, state->saveSlot);
  state->saveSlot = UINT_MAX;
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "cannot cancel index save");
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int saveIndexState(IndexState *state)
{
  int result = prepareToSaveIndexState(state, IS_SAVE);
  if (result != UDS_SUCCESS) {
    return result;
  }
  unsigned int i;
  for (i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    result = writeIndexComponent(component);
    if (result != UDS_SUCCESS) {
      cleanupSave(state);
      return result;
    }
  }
  return completeIndexSaving(state);
}

/*****************************************************************************/
int writeIndexStateCheckpoint(IndexState *state)
{
  int result = prepareToSaveIndexState(state, IS_CHECKPOINT);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int i;
  for (i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if (skipIndexComponentOnCheckpoint(component)) {
      continue;
    }
    result = writeIndexComponent(component);
    if (result != UDS_SUCCESS) {
      cleanupSave(state);
      return result;
    }
  }

  return completeIndexSaving(state);
}

/*****************************************************************************/
int startIndexStateCheckpoint(IndexState *state)
{
  int result = prepareToSaveIndexState(state, IS_CHECKPOINT);
  if (result != UDS_SUCCESS) {
    return result;
  }

  state->saving = true;

  unsigned int i;
  for (i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if (skipIndexComponentOnCheckpoint(component)) {
      continue;
    }
    result = startIndexComponentIncrementalSave(component);
    if (result != UDS_SUCCESS) {
      abortIndexStateCheckpoint(state);
      return result;
    }
  }

  return result;
}

/*****************************************************************************/
int performIndexStateCheckpointChapterSynchronizedSaves(IndexState *state)
{
  if (!state->saving) {
    return UDS_SUCCESS;
  }

  unsigned int i;
  for (i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if (skipIndexComponentOnCheckpoint(component) ||
        !deferIndexComponentCheckpointToChapterWriter(component)) {
      continue;
    }
    int result = performIndexComponentChapterWriterSave(component);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}

/**
 *  Wrapper function to do a zone-based checkpoint operation.
 *
 *  @param [in]  state          the index state
 *  @param [in]  zone           the zone number
 *  @param [in]  compFunc       the index component function to use
 *  @param [out] completed      if non-NULL, where to save the completion status
 *
 *  @return UDS_SUCCESS or an error code
 *
 **/
static int doIndexStateCheckpointInZone(IndexState       *state,
                                        unsigned int      zone,
                                        int (*compFunc)(IndexComponent *,
                                                        unsigned int,
                                                        CompletionStatus *),
                                        CompletionStatus *completed)
{
  if (!state->saving) {
    if (completed != NULL) {
      *completed = CS_COMPLETED_PREVIOUSLY;
    }
    return UDS_SUCCESS;
  }

  CompletionStatus status = CS_COMPLETED_PREVIOUSLY;

  unsigned int i;
  for (i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if (skipIndexComponentOnCheckpoint(component)) {
      continue;
    }
    if (zone > 0 && !component->info->multiZone) {
      continue;
    }
    CompletionStatus componentStatus = CS_NOT_COMPLETED;
    int result = (*compFunc)(component, zone, &componentStatus);
    if (result != UDS_SUCCESS) {
      return result;
    }
    // compute rolling least status
    if (componentStatus < status) {
      status = componentStatus;
    }
  }

  if (completed != NULL) {
    *completed = status;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int performIndexStateCheckpointInZone(IndexState       *state,
                                      unsigned int      zone,
                                      CompletionStatus *completed)
{
  return doIndexStateCheckpointInZone(state, zone,
                                      &performIndexComponentZoneSave,
                                      completed);
}

/*****************************************************************************/
int finishIndexStateCheckpointInZone(IndexState       *state,
                                     unsigned int      zone,
                                     CompletionStatus *completed)
{
  return doIndexStateCheckpointInZone(state, zone,
                                      &finishIndexComponentZoneSave,
                                      completed);
}

/*****************************************************************************/
int abortIndexStateCheckpointInZone(IndexState       *state,
                                    unsigned int      zone,
                                    CompletionStatus *completed)
{
  return doIndexStateCheckpointInZone(state, zone,
                                      &abortIndexComponentZoneSave, completed);
}

/*****************************************************************************/
int finishIndexStateCheckpoint(IndexState *state)
{
  if (!state->saving) {
    return UDS_SUCCESS;
  }

  unsigned int i;
  for (i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if (skipIndexComponentOnCheckpoint(component)) {
      continue;
    }
    int result = finishIndexComponentIncrementalSave(component);
    if (result != UDS_SUCCESS) {
      abortIndexStateCheckpoint(state);
      return result;
    }
  }

  int result = completeIndexSaving(state);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return UDS_SUCCESS;
}

/*****************************************************************************/
int abortIndexStateCheckpoint(IndexState *state)
{
  if (!state->saving) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "not saving the index state");
  }

  logError("aborting index state checkpoint");

  int result = UDS_SUCCESS;
  unsigned int i;
  for (i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if (skipIndexComponentOnCheckpoint(component)) {
      continue;
    }
    int tmp = abortIndexComponentIncrementalSave(component);
    if (result == UDS_SUCCESS) {
      result = tmp;
    }
  }

  cleanupSave(state);
  state->saving = false;

  return result;
}

/*****************************************************************************/
int discardIndexStateData(IndexState *state)
{
  int result = discardIndexSaves(state->layout, true);
  state->saveSlot = UINT_MAX;
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "%s: cannot destroy all index saves",
                                   __func__);
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int discardLastIndexStateSave(IndexState *state)
{
  int result = discardIndexSaves(state->layout, false);
  state->saveSlot = UINT_MAX;
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "%s: cannot destroy latest index save",
                                   __func__);
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
Buffer *getStateIndexStateBuffer(IndexState *state, IOAccessMode mode)
{
  unsigned int slot = mode == IO_READ ? state->loadSlot : state->saveSlot;
  return getIndexStateBuffer(state->layout, slot);
}

/*****************************************************************************/
int openStateBufferedReader(IndexState      *state,
                            RegionKind       kind,
                            unsigned int     zone,
                            BufferedReader **readerPtr)
{
  return openIndexBufferedReader(state->layout, state->loadSlot, kind, zone,
                                 readerPtr);
}

/*****************************************************************************/
int openStateBufferedWriter(IndexState      *state,
                            RegionKind       kind,
                            unsigned int     zone,
                            BufferedWriter **writerPtr)
{
  return openIndexBufferedWriter(state->layout, state->saveSlot, kind, zone,
                                 writerPtr);
}
