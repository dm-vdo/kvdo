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
 * $Id: //eng/uds-releases/krusty/src/uds/indexCheckpoint.c#6 $
 */

#include "indexCheckpoint.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "threads.h"
#include "typeDefs.h"

/**
 * index checkpointState values
 *
 * @note The order of these values is significant,
 *       see indexState.c doIndexStateCheckpointInZone().
 **/
enum checkpoint_state {
  NOT_CHECKPOINTING,
  CHECKPOINT_IN_PROGRESS,
  CHECKPOINT_ABORTING
};

/**
 * Private structure which tracks checkpointing.
 **/
struct index_checkpoint {
  Mutex                 mutex;        // covers this group of fields
  uint64_t              chapter;      // vcn of the starting chapter
  enum checkpoint_state state;        // is checkpoint in progress or aborting
  unsigned int          zonesBusy;    // count of zones not yet done
  unsigned int          frequency;    // number of chapters between checkpoints
  uint64_t              checkpoints;  // number of checkpoints this session
};

/**
 * Enum return value of indexCheckpointTrigger function.
 **/
enum index_checkpoint_trigger_value {
  ICTV_IDLE,       //< no checkpointing right now
  ICTV_START,      //< start a new checkpoint now
  ICTV_CONTINUE,   //< continue checkpointing if needed
  ICTV_FINISH,     //< finish checkpointing, next time will start new cycle
  ICTV_ABORT       //< immediately abort checkpointing
};

typedef int CheckpointFunction(struct index *index, unsigned int zone);

//  These functions are called while holding the checkpoint->mutex but are
//  expected to release it.
//
static CheckpointFunction doCheckpointStart;
static CheckpointFunction doCheckpointProcess;
static CheckpointFunction doCheckpointFinish;
static CheckpointFunction doCheckpointAbort;

CheckpointFunction *const checkpointFuncs[] = {
  NULL,
  doCheckpointStart,
  doCheckpointProcess,
  doCheckpointFinish,
  doCheckpointAbort
};

/**********************************************************************/
int makeIndexCheckpoint(struct index *index)
{
  struct index_checkpoint *checkpoint;
  int result = ALLOCATE(1, struct index_checkpoint, "struct index_checkpoint",
                        &checkpoint);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initMutex(&checkpoint->mutex);
  if (result != UDS_SUCCESS) {
    FREE(checkpoint);
    return result;
  }

  checkpoint->checkpoints = 0;

  index->checkpoint = checkpoint;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeIndexCheckpoint(struct index_checkpoint *checkpoint)
{
  if (checkpoint != NULL) {
    destroyMutex(&checkpoint->mutex);
    FREE(checkpoint);
  }
}

/**********************************************************************/
unsigned int getIndexCheckpointFrequency(struct index_checkpoint *checkpoint)
{
  lockMutex(&checkpoint->mutex);
  unsigned int frequency = checkpoint->frequency;
  unlockMutex(&checkpoint->mutex);
  return frequency;
}

/**********************************************************************/
unsigned int setIndexCheckpointFrequency(struct index_checkpoint *checkpoint,
                                         unsigned int             frequency)
{
  lockMutex(&checkpoint->mutex);
  unsigned int oldFrequency = checkpoint->frequency;
  checkpoint->frequency = frequency;
  unlockMutex(&checkpoint->mutex);
  return oldFrequency;
}

/**********************************************************************/
uint64_t getCheckpointCount(struct index_checkpoint *checkpoint)
{
  return checkpoint->checkpoints;
}

/**********************************************************************/
static enum index_checkpoint_trigger_value
getCheckpointAction(struct index_checkpoint *checkpoint,
                    uint64_t virtualChapter)
{
  if (checkpoint->frequency == 0) {
    return ICTV_IDLE;
  }
  unsigned int value = virtualChapter % checkpoint->frequency;
  if (checkpoint->state == CHECKPOINT_ABORTING) {
    return ICTV_ABORT;
  } else if (checkpoint->state == CHECKPOINT_IN_PROGRESS) {
    if (value == checkpoint->frequency - 1) {
      return ICTV_FINISH;
    } else {
      return ICTV_CONTINUE;
    }
  } else {
    if (value == 0) {
      return ICTV_START;
    } else {
      return ICTV_IDLE;
    }
  }
}

/**********************************************************************/
int processCheckpointing(struct index *index,
                         unsigned int  zone,
                         uint64_t      newVirtualChapter)
{
  struct index_checkpoint *checkpoint = index->checkpoint;
  lockMutex(&checkpoint->mutex);

  enum index_checkpoint_trigger_value ictv
    = getCheckpointAction(checkpoint, newVirtualChapter);

  if (ictv == ICTV_START) {
    checkpoint->chapter = newVirtualChapter;
  }

  CheckpointFunction *func = checkpointFuncs[ictv];
  if (func == NULL) {
    // nothing to do in idle state
    unlockMutex(&checkpoint->mutex);
    return UDS_SUCCESS;
  }

  return (*func)(index, zone);
}

/**********************************************************************/
int processChapterWriterCheckpointSaves(struct index *index)
{
  struct index_checkpoint *checkpoint = index->checkpoint;

  int result = UDS_SUCCESS;

  lockMutex(&checkpoint->mutex);
  if (checkpoint->state == CHECKPOINT_IN_PROGRESS) {
    result =
      perform_index_state_checkpoint_chapter_synchronized_saves(index->state);

    if (result != UDS_SUCCESS) {
      checkpoint->state = CHECKPOINT_ABORTING;
      logInfo("checkpoint failed");
      index->last_checkpoint = index->prev_checkpoint;
    }
  }

  unlockMutex(&checkpoint->mutex);
  return result;
}

/**
 *  Helper function used to abort checkpoint if an error has occurred.
 *
 *  @param index        the index
 *  @param result       the error result
 *
 *  @return result
 **/
static int abortCheckpointing(struct index *index, int result)
{
  if (index->checkpoint->state != NOT_CHECKPOINTING) {
    index->checkpoint->state = CHECKPOINT_ABORTING;
    logInfo("checkpoint failed");
    index->last_checkpoint = index->prev_checkpoint;
  }
  return result;
}

/**********************************************************************/
int finishCheckpointing(struct index *index)
{
  struct index_checkpoint *checkpoint = index->checkpoint;

  int result = processChapterWriterCheckpointSaves(index);
  if (result != UDS_SUCCESS) {
    return result;
  }

  lockMutex(&checkpoint->mutex);

  unsigned int z;
  for (z = 0; z < index->zone_count; ++z) {
    if (checkpoint->state != CHECKPOINT_IN_PROGRESS) {
      break;
    }
    result = doCheckpointFinish(index, z);
    // reacquire mutex released by doCheckpointFinish
    lockMutex(&checkpoint->mutex);
    if (result != UDS_SUCCESS) {
      break;
    }
  }

  if ((result == UDS_SUCCESS) &&
      (checkpoint->state == CHECKPOINT_IN_PROGRESS)) {
    result = finish_index_state_checkpoint(index->state);
    if (result == UDS_SUCCESS) {
      checkpoint->state = NOT_CHECKPOINTING;
    }
  }

  unlockMutex(&checkpoint->mutex);
  return result;
}

/**
 * Starts an incremental checkpoint.
 *
 * Called by the first zone to finish a chapter which starts a checkpoint.
 *
 * @param index the index
 * @param zone  the zone number
 *
 * @return UDS_SUCCESS or an error code
 **/
static int doCheckpointStart(struct index *index, unsigned int zone)
{
  struct index_checkpoint *checkpoint = index->checkpoint;
  begin_save(index, true, checkpoint->chapter);
  int result = start_index_state_checkpoint(index->state);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "cannot start index checkpoint");
    index->last_checkpoint = index->prev_checkpoint;
    unlockMutex(&checkpoint->mutex);
    return result;
  }

  checkpoint->state = CHECKPOINT_IN_PROGRESS;
  checkpoint->zonesBusy = index->zone_count;

  return doCheckpointProcess(index, zone);
}

/**********************************************************************/
static int doCheckpointProcess(struct index *index, unsigned int zone)
{
  struct index_checkpoint *checkpoint = index->checkpoint;
  unlockMutex(&checkpoint->mutex);
  enum completion_status status = CS_NOT_COMPLETED;
  int result
    = perform_index_state_checkpoint_in_zone(index->state, zone, &status);
  if (result != UDS_SUCCESS) {
    lockMutex(&checkpoint->mutex);
    logErrorWithStringError(result, "cannot continue index checkpoint");
    result = abortCheckpointing(index, result);
    unlockMutex(&checkpoint->mutex);
  } else if (status == CS_JUST_COMPLETED) {
    lockMutex(&checkpoint->mutex);
    if (--checkpoint->zonesBusy == 0) {
      checkpoint->checkpoints += 1;
      logInfo("finished checkpoint");
      result = finish_index_state_checkpoint(index->state);
      if (result != UDS_SUCCESS) {
        logErrorWithStringError(result, "%s checkpoint finish failed",
                                __func__);
      }
      checkpoint->state = NOT_CHECKPOINTING;
    }
    unlockMutex(&checkpoint->mutex);
  }
  return result;
}

/**********************************************************************/
static int doCheckpointAbort(struct index *index, unsigned int zone)
{
  struct index_checkpoint *checkpoint = index->checkpoint;
  enum completion_status status = CS_NOT_COMPLETED;
  int result
    = abort_index_state_checkpoint_in_zone(index->state, zone, &status);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "cannot abort index checkpoint");
  } else if (status == CS_JUST_COMPLETED) {
    if (--checkpoint->zonesBusy == 0) {
      logInfo("aborted checkpoint");
      result = abort_index_state_checkpoint(index->state);
      if (result != UDS_SUCCESS) {
        logErrorWithStringError(result, "checkpoint abort failed");
      }
      checkpoint->state = NOT_CHECKPOINTING;
    }
  }
  unlockMutex(&checkpoint->mutex);

  return result;
}

/**********************************************************************/
static int doCheckpointFinish(struct index *index, unsigned int zone)
{
  struct index_checkpoint *checkpoint = index->checkpoint;
  enum completion_status status = CS_NOT_COMPLETED;
  unlockMutex(&checkpoint->mutex);
  int result
    = finish_index_state_checkpoint_in_zone(index->state, zone, &status);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "cannot finish index checkpoint");
    lockMutex(&checkpoint->mutex);
    result = abortCheckpointing(index, result);
    unlockMutex(&checkpoint->mutex);
  } else if (status == CS_JUST_COMPLETED) {
    lockMutex(&checkpoint->mutex);
    if (--checkpoint->zonesBusy == 0) {
      checkpoint->checkpoints += 1;
      logInfo("finished checkpoint");
      result = finish_index_state_checkpoint(index->state);
      if (result != UDS_SUCCESS) {
        logErrorWithStringError(result, "%s checkpoint finish failed",
                                __func__);
      }
      checkpoint->state = NOT_CHECKPOINTING;
    }
    unlockMutex(&checkpoint->mutex);
  }
  return result;
}
