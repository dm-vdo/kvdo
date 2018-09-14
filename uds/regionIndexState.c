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
 * $Id: //eng/uds-releases/gloria/src/uds/regionIndexState.c#3 $
 */

#include "regionIndexStateInternal.h"

#include "bufferedIORegion.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "regionIndexComponent.h"
#include "singleFileLayoutInternals.h"
#include "typeDefs.h"

static const IndexStateOps *getRegionIndexStateOps(void);

/*****************************************************************************/
int makeRegionIndexState(SingleFileLayout  *sfl,
                         unsigned int       zoneCount,
                         unsigned int       length,
                         IndexState       **statePtr)
{
  RegionIndexState *ris = NULL;
  int result = ALLOCATE(1, RegionIndexState, "region index state", &ris);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initIndexState(&ris->state, 0, zoneCount, length,
                          getRegionIndexStateOps());
  if (result != UDS_SUCCESS) {
    FREE(ris);
    return result;
  }

  ris->sfl       = sfl;
  ris->loadZones = 0;
  ris->loadSlot  = UINT_MAX;
  ris->saveSlot  = UINT_MAX;

  *statePtr = &ris->state;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static void ris_freeMe(IndexState *state)
{
  if (state == NULL) {
    return;
  }
  RegionIndexState *ris = asRegionIndexState(state);
  destroyIndexState(&ris->state);
  FREE(ris);
}

/*****************************************************************************/
static int ris_addComponent(IndexState               *state,
                            const IndexComponentInfo *info,
                            void                     *data,
                            void                     *context)
{
  RegionIndexState *ris = asRegionIndexState(state);

  IndexComponent *component = NULL;
  int result = makeRegionIndexComponent(ris, info, state->zoneCount,
                                        data, context, &component);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "cannot make region index component");
  }

  result = addComponentToIndexState(&ris->state, component);
  if (result != UDS_SUCCESS) {
    freeIndexComponent(&component);
    return result;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int ris_loadState(IndexState *state, bool *replayPtr)
{
  RegionIndexState *ris = asRegionIndexState(state);
  int result = ASSERT((state->id == 0), "Cannot have multiple subindices");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = findLatestIndexSaveSlot(ris->sfl, &ris->loadZones, &ris->loadSlot);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = genericLoadIndexState(state, replayPtr);
  ris->loadZones = 0;
  ris->loadSlot  = UINT_MAX;
  return result;
}

/*****************************************************************************/
static int ris_prepareSave(IndexState *state, IndexSaveType saveType)
{
  RegionIndexState *ris = asRegionIndexState(state);
  int result = ASSERT((state->id == 0), "Cannot have multiple subindices");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = setupSingleFileIndexSaveSlot(ris->sfl, state->zoneCount, saveType,
                                        &ris->saveSlot);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "%s: cannot prepare index %s",
                                   indexSaveTypeName(saveType), __func__);
  }

  return UDS_SUCCESS;
}

/*****************************************************************************/
static int ris_commitSave(IndexState *state)
{
  RegionIndexState *ris = asRegionIndexState(state);
  int result = ASSERT((state->id == 0), "Cannot have multiple subindices");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = commitSingleFileIndexSave(ris->sfl, ris->saveSlot);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "%s: cannot commit index save",
                                   __func__);
  }

  ris->saveSlot = UINT_MAX;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int ris_cleanupSave(IndexState *state)
{
  RegionIndexState *ris = asRegionIndexState(state);
  int result = ASSERT((state->id == 0), "Cannot have multiple subindices");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = cancelSingleFileIndexSave(ris->sfl, ris->saveSlot);
  ris->saveSlot = UINT_MAX;
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "%s: cannot cancel index save",
                                   __func__);
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int ris_writeSingleComponent(IndexState     *state
                                    __attribute__((unused)),
                                    IndexComponent *component
                                    __attribute__((unused)))
{
  return UDS_UNSUPPORTED;
}

/*****************************************************************************/
static int ris_discardSaves(IndexState *state, DiscardType dt)
{
  RegionIndexState *ris = asRegionIndexState(state);
  int result = ASSERT((state->id == 0), "Cannot have multiple subindices");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = discardSingleFileIndexSaves(ris->sfl, dt == DT_DISCARD_ALL);
  ris->saveSlot = UINT_MAX;
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "%s: cannot destroy %s", __func__,
                                   ((dt == DT_DISCARD_ALL)
                                    ? "all index saves"
                                    : "latest index save"));
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/

static const IndexStateOps regionIndexStateOps = {
  .freeFunc             = ris_freeMe,
  .addComponent         = ris_addComponent,
  .loadState            = ris_loadState,
  .saveState            = genericSaveIndexState,
  .prepareSave          = ris_prepareSave,
  .commitSave           = ris_commitSave,
  .cleanupSave          = ris_cleanupSave,
  .writeCheckpoint      = genericWriteIndexStateCheckpoint,
  .writeSingleComponent = ris_writeSingleComponent,
  .discardSaves         = ris_discardSaves,
};

static const IndexStateOps *getRegionIndexStateOps(void)
{
  return &regionIndexStateOps;
}

/*****************************************************************************/
int openRegionStateRegion(RegionIndexState  *ris,
                          IOAccessMode       mode,
                          RegionKind         kind,
                          unsigned int       zone,
                          IORegion         **regionPtr)
{
  unsigned int  slot;
  const char   *operation;

  if (mode == IO_READ) {
    slot      = ris->loadSlot;
    operation = "load";
  } else if (mode == IO_WRITE) {
    slot      = ris->saveSlot;
    operation = "save";
  } else {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "%s: only IO_READ and IO_WRITE valid",
                                   __func__);
  }

  int result = ASSERT((ris->state.id == 0), "Cannot have multiple subindices");
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT((slot < ris->sfl->super.maxSaves), "%s not started",
                  operation);
  if (result != UDS_SUCCESS) {
    return result;
  }

  IndexSaveLayout *isl = &ris->sfl->index.saves[slot];

  LayoutRegion *lr = NULL;
  switch (kind) {
    case RL_KIND_INDEX_PAGE_MAP:
      lr = &isl->indexPageMap;
      break;

    case RL_KIND_OPEN_CHAPTER:
      if (isl->openChapter == NULL) {
        return logErrorWithStringError(UDS_UNEXPECTED_RESULT,
                                       "%s: %s has no open chapter",
                                       __func__, operation);
      }
      lr = isl->openChapter;
      break;

    case RL_KIND_MASTER_INDEX:
      if (isl->masterIndexZones == NULL || zone >= isl->numZones) {
        return logErrorWithStringError(UDS_UNEXPECTED_RESULT,
                                       "%s: %s has no master index zone %u",
                                       __func__, operation, zone);
      }
      lr = &isl->masterIndexZones[zone];
      break;

    case RL_KIND_INDEX_STATE:
      if (isl->indexStateBuffer == NULL) {
        return logErrorWithStringError(UDS_UNEXPECTED_RESULT,
                                       "%s: %s has no index state buffer",
                                       __func__, operation);
      }
      return makeBufferedRegion(isl->indexStateBuffer, 0, regionPtr);
      break;

    default:
      return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                     "%s: unexpected kind %u",
                                     __func__, kind);
  }

  return getSingleFileLayoutRegion(ris->sfl, lr, mode, regionPtr);
}
