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
 * $Id: //eng/uds-releases/jasper/src/uds/indexState.h#1 $
 */

#ifndef INDEX_STATE_H
#define INDEX_STATE_H 1

#include "accessMode.h"
#include "indexComponent.h"

/**
 * Used here and in SingleFileLayout.
 **/
typedef enum {
  IS_SAVE,
  IS_CHECKPOINT,
  NO_SAVE = 9999,
} IndexSaveType;

/**
 * The index state structure controls the loading and saving of the index
 * state.
 **/
typedef struct indexState {
  struct indexLayout *layout;
  unsigned int        zoneCount;  // number of index zones to use
  unsigned int        loadZones;
  unsigned int        loadSlot;
  unsigned int        saveSlot;
  unsigned int        count;     // count of registered entries (<= length)
  unsigned int        length;    // total span of array allocation
  bool                saving;    // incremental save in progress
  IndexComponent     *entries[]; // array of index component entries
} IndexState;

/**
 * Make an index state object,
 *
 * @param [in]  layout         The index layout.
 * @param [in]  numZones       The number of zones to use.
 * @param [in]  maxComponents  The maximum number of components to be handled.
 * @param [out] statePtr       Where to store the index state object.
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeIndexState(struct indexLayout  *layout,
                   unsigned int         numZones,
                   unsigned int         maxComponents,
                   IndexState         **statePtr)
  __attribute__((warn_unused_result));

/**
 * Free an index state (generically).
 *
 * @param statePtr      The pointer to the index state to be freed and
 *                      set to NULL.
 **/
void freeIndexState(IndexState **statePtr);

/**
 * Add an index component to an index state.
 *
 * @param state     The index directory in which to add this component.
 * @param info      The index component file specification.
 * @param data      The per-component data structure.
 * @param context   The load/save context of the component.
 *
 * @return          UDS_SUCCESS or an error code.
 **/
int addIndexStateComponent(IndexState               *state,
                           const IndexComponentInfo *info,
                           void                     *data,
                           void                     *context)
  __attribute__((warn_unused_result));

/**
 * Load index state
 *
 * @param state      The index state.
 * @param replayPtr  If set, the place to hold whether a replay is required.
 *
 * @return           UDS_SUCCESS or error
 **/
int loadIndexState(IndexState *state, bool *replayPtr)
  __attribute__((warn_unused_result));

/**
 * Save the current index state, including the open chapter.
 *
 * @param state         The index state.
 *
 * @return              UDS_SUCCESS or error
 **/
int saveIndexState(IndexState *state) __attribute__((warn_unused_result));

/**
 *  Prepare to save the index state.
 *
 *  @param state     the index state
 *  @param saveType  whether a checkpoint or save
 *
 *  @return UDS_SUCCESS or an error code
 **/
int prepareToSaveIndexState(IndexState *state, IndexSaveType saveType)
  __attribute__((warn_unused_result));

/**
 * Write index checkpoint non-incrementally (for testing).
 *
 * @param state         The index state.
 *
 * @return              UDS_SUCCESS or error
 **/
int writeIndexStateCheckpoint(IndexState *state)
  __attribute__((warn_unused_result));

/**
 * Sets up an index state checkpoint which will proceed incrementally.
 * May create the directory but does not actually write any data.
 *
 * @param state         The index state.
 *
 * @return              UDS_SUCCESS or an error code.
 **/
int startIndexStateCheckpoint(IndexState *state)
  __attribute__((warn_unused_result));

/**
 * Perform operations on index state checkpoints that are synchronized to
 * the chapter writer thread.
 *
 * @param state         The index state.
 *
 * @return              UDS_SUCCESS or an error code.
 **/
int performIndexStateCheckpointChapterSynchronizedSaves(IndexState *state)
  __attribute__((warn_unused_result));

/**
 * Performs zone-specific (and, for zone 0, general) incremental checkpointing.
 *
 * @param [in]  state           The index state.
 * @param [in]  zone            The zone number.
 * @param [out] completed       Set to whether the checkpoint has completed
 *                              for this zone.
 *
 * @return              UDS_SUCCESS or an error code.
 **/
int performIndexStateCheckpointInZone(IndexState       *state,
                                      unsigned int      zone,
                                      CompletionStatus *completed)
  __attribute__((warn_unused_result));

/**
 * Force the completion of an incremental index state checkpoint
 * for a particular zone.
 *
 * @param [in] state    The index state.
 * @param [in]  zone            The zone number.
 * @param [out] completed       Set to whether the checkpoint has completed
 *                              for this zone.
 *
 * @return              UDS_SUCCESS or an error code.
 **/
int finishIndexStateCheckpointInZone(IndexState       *state,
                                     unsigned int      zone,
                                     CompletionStatus *completed)
  __attribute__((warn_unused_result));

/**
 * Force the completion of an incremental index state checkpoint once
 * all zones are completed.
 *
 * @param [in] state    The index state.
 *
 * @return              UDS_SUCCESS or an error code.
 **/
int finishIndexStateCheckpoint(IndexState *state)
  __attribute__((warn_unused_result));

/**
 * Aborts an index state checkpoint which is proceeding incrementally
 * for a particular zone.
 *
 * @param [in]  state           The index state.
 * @param [in]  zone            The zone number.
 * @param [out] completed       Set to whether the checkpoint has completed or
 *                              aborted for this zone.
 *
 * @return              UDS_SUCCESS or an error code.
 **/
int abortIndexStateCheckpointInZone(IndexState       *state,
                                    unsigned int      zone,
                                    CompletionStatus *completed);

/**
 * Aborts an index state checkpoint which is proceeding incrementally,
 * once all the zones are aborted.
 *
 * @param [in]  state   The index state.
 *
 * @return              UDS_SUCCESS or an error code.
 **/
int abortIndexStateCheckpoint(IndexState *state);

/**
 * Remove or disable the index state data, for testing.
 *
 * @param state         The index state
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note the return value of this function is frequently ignored
 **/
int discardIndexStateData(IndexState *state);

/**
 * Discard the last index state save, for testing.
 *
 * @param state         The index state
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note the return value of this function is frequently ignored
 **/
int discardLastIndexStateSave(IndexState *state);

/**
 * Find index component, for testing.
 *
 * @param state The index state
 * @param info  The index component file specification
 *
 * @return      The index component, or NULL if not found
 **/
IndexComponent *findIndexComponent(const IndexState         *state,
                                   const IndexComponentInfo *info)
  __attribute__((warn_unused_result));

/**
 * Open an IORegion for a specified state, mode, kind, and zone.
 * This helper function is used by RegionIndexComponent.
 *
 * @param state      The index state.
 * @param mode       One of IO_READ or IO_WRITE.
 * @param kind       The kind if index save region to open.
 * @param zone       The zone number for the region.
 * @param regionPtr  Where to store the region.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int openStateRegion(IndexState    *state,
                    IOAccessMode   mode,
                    RegionKind     kind,
                    unsigned int   zone,
                    IORegion     **regionPtr)
  __attribute__((warn_unused_result));

#endif // INDEX_STATE_H
