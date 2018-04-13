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
 * $Id: //eng/uds-releases/gloria/src/uds/indexCheckpoint.h#1 $
 */

#ifndef INDEX_CHECKPOINT_H
#define INDEX_CHECKPOINT_H

#include "index.h"

/**
 * Construct and initialize the checkpoint sub-structure of an index.
 *
 * @param index  the index receive the new checkpoint structure.
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeIndexCheckpoint(Index *index) __attribute__((warn_unused_result));

/**
 * Free the checkpoint sub-structure of an index.
 *
 * @param checkpoint  the structure to free
 **/
void freeIndexCheckpoint(IndexCheckpoint *checkpoint);

/**
 * Get the current checkpointing frequency of an index.
 *
 * @param checkpoint  the checkpoint state of the index
 *
 * @return the number of chapters between checkpoints
 **/
unsigned int getIndexCheckpointFrequency(IndexCheckpoint *checkpoint)
  __attribute__((warn_unused_result));

/**
 * Set checkpointing frequency for the index.
 *
 * @param checkpoint  the checkpoint state of the index
 * @param frequency   The new checkpointing frequency
 *
 * @return the old checkpointing frequency
 **/
unsigned int setIndexCheckpointFrequency(IndexCheckpoint *checkpoint,
                                         unsigned int     frequency);

/**
 * Gets the number of checkpoints completed during the lifetime of this index
 *
 * @param checkpoint  the checkpoint state of the index
 *
 * @return            the number of checkpoints completed
 **/
uint64_t getCheckpointCount(IndexCheckpoint *checkpoint)
  __attribute__((warn_unused_result));

/**
 * If incremental checkpointing is in progress, finish it.
 *
 * @param index     The index
 *
 * @return          UDS_SUCCESS or an error code
 *
 * @note        This function is called automatically during normal operation;
 *              its presence here is for tests that expect checkpointing to
 *              have completed at some point in their logic.  It is not an
 *              error to call this function if checkpointing is not in
 *              progress, it silently returns success.
 **/
int finishCheckpointing(Index *index) __attribute__((warn_unused_result));

/**
 * Process one zone's incremental checkpoint operation. Automatically
 * starts, processes, and finishes a checkpoint over multiple invocations
 * as successive chapters are closed and written.
 *
 * Uses its own mutex to serialize the starting and finishing or aborting,
 * but allows parallel execution of the incremental progress.
 *
 * @param index             The index to checkpoint
 * @param zone              The current zone number
 * @param newVirtualChapter The number of the chapter which the calling
 *                          zone has just opened
 *
 * @return UDS_SUCCESS or an error code.
 **/
int processCheckpointing(Index        *index,
                         unsigned int  zone,
                         uint64_t      newVirtualChapter)
  __attribute__((warn_unused_result));

/**
 * Process saves done outside any zone by the chapter writer.
 *
 * Grabs the mutex associated with processCheckpointing().
 *
 * @param index         The index to process.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int processChapterWriterCheckpointSaves(Index *index)
  __attribute__((warn_unused_result));

#endif // INDEX_CHECKPOINT_H
