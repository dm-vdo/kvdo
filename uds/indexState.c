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
 * $Id: //eng/uds-releases/gloria/src/uds/indexState.c#1 $
 */

#include "indexStateInternals.h"

#include "errors.h"
#include "indexComponentInternal.h"
#include "logger.h"
#include "memoryAlloc.h"

/**********************************************************************/
int initIndexState(IndexState          *state,
                   unsigned int         id,
                   unsigned int         zoneCount,
                   unsigned int         length,
                   const IndexStateOps *ops)
{
  if (length == 0) {
    return logErrorWithStringError(
      UDS_INVALID_ARGUMENT, "cannot make index state with length 0");
  }

  state->id        = id;
  state->zoneCount = zoneCount;
  state->count     = 0;
  state->length    = length;
  state->saving    = false;
  state->ops       = ops;

  return ALLOCATE(state->length, IndexComponent *, "index state entries",
                  &state->entries);
}

/**********************************************************************/
void destroyIndexState(IndexState *state)
{
  if (state != NULL) {
    for (unsigned int i = 0; i < state->count; ++i) {
      freeIndexComponent(&state->entries[i]);
    }
    FREE(state->entries);
  }
}

/**********************************************************************/
void freeIndexState(IndexState **statePtr)
{
  if (*statePtr != NULL) {
    (*statePtr)->ops->freeFunc(*statePtr);
    *statePtr = NULL;
  }
}

/**********************************************************************/
int addIndexStateComponent(IndexState               *state,
                           const IndexComponentInfo *info,
                           void                     *data,
                           void                     *context)
{
  return state->ops->addComponent(state, info, data, context);
}

/*****************************************************************************/
int addComponentToIndexState(IndexState     *state,
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

/**********************************************************************/
IndexComponent *findIndexComponent(const IndexState         *state,
                                   const IndexComponentInfo *info)
{
  for (unsigned int i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if ((info == component->info)
        || (strcmp(info->fileName, component->info->fileName) == 0)) {
      return component;
    }
  }
  return NULL;
}
/**********************************************************************/
int loadIndexState(IndexState *state,
                   bool       *replayPtr)
{
  return state->ops->loadState(state, replayPtr);
}

/**
 *  Prepare to save the index state.
 *
 *  @param state        the index state
 *  @param type         whether a checkpoint or save
 *
 *  @return UDS_SUCCESS or an error code
 *
 *  @note mostly this waits for previous async operations to complete,
 *        removes the deletion directory as well as any partially-saved
 *        next state directory, and then makes a new empty next state directory.
 **/
static int prepareToSave(IndexState *state, IndexSaveType type)
{
  if (state->saving) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "already saving the index state");
  }
  return state->ops->prepareSave(state, type);
}

/************************************************************************/
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

  int result = state->ops->commitSave(state);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "cannot commit index state");
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int genericLoadIndexState(IndexState *state,
                          bool       *replayPtr)
{
  bool replayRequired = false;
  for (unsigned int i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    int result = readIndexComponent(component);
    if (result != UDS_SUCCESS) {
      if (!missingIndexComponentRequiresReplay(component)) {
        return logErrorWithStringError(result,
                                       "index component %s",
                                       indexComponentName(component));
      }
      replayRequired = true;
    }
  }

  if (replayPtr != NULL) {
    *replayPtr = replayRequired;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int genericSaveIndexState(IndexState *state)
{
  int result = UDS_SUCCESS;
  for (unsigned int i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    result = writeIndexComponent(component);
    if (result != UDS_SUCCESS) {
      break;
    }
  }
  return result;
}

/**********************************************************************/
int genericWriteIndexStateCheckpoint(IndexState *state)
{
  int result = UDS_SUCCESS;
  for (unsigned int i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if (skipIndexComponentOnCheckpoint(component)) {
      continue;
    }
    result = writeIndexComponent(component);
    if (result != UDS_SUCCESS) {
      break;
    }
  }

  return result;
}

/**********************************************************************/
int saveIndexState(IndexState *state)
{
  int result = prepareToSave(state, IS_SAVE);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = state->ops->saveState(state);
  if (result != UDS_SUCCESS) {
    state->ops->cleanupSave(state);
    return result;
  }

  result = completeIndexSaving(state);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
int writeIndexStateCheckpoint(IndexState *state)
{
  int result = prepareToSave(state, IS_CHECKPOINT);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = state->ops->writeCheckpoint(state);
  if (result != UDS_SUCCESS) {
    state->ops->cleanupSave(state);
    return result;
  }

  result = completeIndexSaving(state);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
int startIndexStateCheckpoint(IndexState *state)
{
  int result = prepareToSave(state, IS_CHECKPOINT);
  if (result != UDS_SUCCESS) {
    return result;
  }

  state->saving = true;

  for (unsigned int i = 0; i < state->count; ++i) {
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

/**********************************************************************/
int performIndexStateCheckpointChapterSynchronizedSaves(IndexState *state)
{
  if (!state->saving) {
    return UDS_SUCCESS;
  }

  for (unsigned int i = 0; i < state->count; ++i) {
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

  for (unsigned int i = 0; i < state->count; ++i) {
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

/**********************************************************************/
int performIndexStateCheckpointInZone(IndexState       *state,
                                      unsigned int      zone,
                                      CompletionStatus *completed)
{
  return doIndexStateCheckpointInZone(state, zone,
                                      &performIndexComponentZoneSave,
                                      completed);
}

/**********************************************************************/
int finishIndexStateCheckpointInZone(IndexState       *state,
                                     unsigned int      zone,
                                     CompletionStatus *completed)
{
  return doIndexStateCheckpointInZone(state, zone,
                                      &finishIndexComponentZoneSave,
                                      completed);
}

/**********************************************************************/
int abortIndexStateCheckpointInZone(IndexState       *state,
                                    unsigned int      zone,
                                    CompletionStatus *completed)
{
  return doIndexStateCheckpointInZone(state, zone,
                                      &abortIndexComponentZoneSave,
                                      completed);
}

/**********************************************************************/
int finishIndexStateCheckpoint(IndexState *state)
{
  if (!state->saving) {
    return UDS_SUCCESS;
  }

  for (unsigned int i = 0; i < state->count; ++i) {
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

/**********************************************************************/
int abortIndexStateCheckpoint(IndexState *state)
{
  if (!state->saving) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "not saving the index state");
  }

  logError("aborting index state checkpoint");

  int result = UDS_SUCCESS;
  for (unsigned int i = 0; i < state->count; ++i) {
    IndexComponent *component = state->entries[i];
    if (skipIndexComponentOnCheckpoint(component)) {
      continue;
    }
    int tmp = abortIndexComponentIncrementalSave(component);
    if (result == UDS_SUCCESS) {
      result = tmp;
    }
  }

  state->ops->cleanupSave(state);
  state->saving = false;

  return result;
}

/**********************************************************************/
int writeSingleIndexStateComponent(IndexState               *state,
                                   const IndexComponentInfo *info)
{
  IndexComponent *component = findIndexComponent(state, info);

  if (component == NULL) {
    return UDS_INVALID_ARGUMENT;
  }

  int result = prepareToSave(state, IS_SAVE);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return state->ops->writeSingleComponent(state, component);
}

/**********************************************************************/
int discardIndexStateData(IndexState *state)
{
  return state->ops->discardSaves(state, DT_DISCARD_ALL);
}

/**********************************************************************/
int discardLastIndexStateSave(IndexState *state)
{
  return state->ops->discardSaves(state, DT_DISCARD_LATEST);
}

/**********************************************************************/
const char *indexSaveTypeName(IndexSaveType saveType)
{
  return saveType == IS_SAVE ? "save" : "checkpoint";
}
