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
 * $Id: //eng/uds-releases/gloria/src/uds/indexStateInternals.h#1 $
 */

#ifndef INDEX_STATE_INTERNALS_H
#define INDEX_STATE_INTERNALS_H

#include "indexState.h"

typedef enum {
  DT_DISCARD_LATEST,
  DT_DISCARD_ALL,
} DiscardType;

struct indexStateOps {
  void (*freeFunc)(IndexState *);
  int  (*addComponent)(IndexState *, const IndexComponentInfo *, void *,
                       void *);
  int  (*loadState)(IndexState *, bool *);
  int  (*saveState)(IndexState *);
  int  (*prepareSave)(IndexState *, IndexSaveType);
  int  (*commitSave)(IndexState *);
  int  (*cleanupSave)(IndexState *);
  int  (*writeCheckpoint)(IndexState *);
  int  (*writeSingleComponent)(IndexState *, IndexComponent *);
  int  (*discardSaves)(IndexState *, DiscardType);
};

/**
 * Initialize an index state structure.
 *
 * @param state         The index state structure to initialize.
 * @param id            The sub-index id for this index.
 * @param zoneCount     The number of index zones in use.
 * @param length        Number of components to hold.
 * @param ops           The operations table for this index state type.
 *
 * @return              UDS_SUCCESS or a failure code.
 **/
int initIndexState(IndexState          *state,
                   unsigned int         id,
                   unsigned int         zoneCount,
                   unsigned int         length,
                   const IndexStateOps *ops)
  __attribute__((warn_unused_result));

/**
 * Destroy the common index state.
 *
 * @param state         The index state sub-structure
 **/
void destroyIndexState(IndexState *state);

/**
 * Internal function to add a component to the index state.
 *
 * @param state         The index state.
 * @param component     The index component.
 *
 * @return      UDS_SUCCESS or an error code.
 **/
int addComponentToIndexState(IndexState     *state,
                             IndexComponent *component)
  __attribute__((warn_unused_result));

/**
 * Load the index state by iterating over each component and reading it.
 *
 * @param state         The index state.
 * @param replayPtr     Whether a replay will be required.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int genericLoadIndexState(IndexState *state,
                          bool       *replayPtr)
  __attribute__((warn_unused_result));

/**
 * Save the index state by iterating over each component and saving it.
 *
 * @param state         The index state.
 *
 * @return UDS_SUCCESS or an error code
 **/
int genericSaveIndexState(IndexState *state)
  __attribute__((warn_unused_result));

/**
 * Write a checkpoint by iterating over each appropriate component and
 * saving it.
 *
 * @param state         The index state.
 *
 * @return UDS_SUCCESS or an error code
 **/
int genericWriteIndexStateCheckpoint(IndexState *state)
  __attribute__((warn_unused_result));

/**
 * Return the name of the index save type.
 *
 * @param saveType      An index save type.
 *
 * @return the name of the type
 **/
const char *indexSaveTypeName(IndexSaveType saveType);

#endif // INDEX_STATE_INTERNALS_H
