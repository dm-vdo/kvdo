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
 * $Id: //eng/uds-releases/gloria/src/uds/indexCheckpoint.c#2 $
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
typedef enum checkpointState {
  NOT_CHECKPOINTING,
  CHECKPOINT_IN_PROGRESS,
  CHECKPOINT_ABORTING
} CheckpointState;

/**
 * Private structure which tracks checkpointing.
 **/
struct indexCheckpoint {
  Mutex            mutex;       // covers this group of fields
  uint64_t         chapter;     // vcn of the starting chapter
  CheckpointState  state;       // is checkpoint in progress or aborting
  unsigned int     zonesBusy;   // count of zones not yet done
  unsigned int     frequency;   // number of chapters between checkpoints
  uint64_t         checkpoints; // number of checkpoints this session
};

/**
 * Enum return value of indexCheckpointTrigger function.
 **/
typedef enum indexCheckpointTriggerValue {
  ICTV_IDLE,       //< no checkpointing right now
  ICTV_START,      //< start a new checkpoint now
  ICTV_CONTINUE,   //< continue checkpointing if needed
  ICTV_FINISH,     //< finish checkpointing, next time will start new cycle
  ICTV_ABORT       //< immediately abort checkpointing
} IndexCheckpointTriggerValue;

typedef int CheckpointFunction(Index *index, unsigned int zone);

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
int makeIndexCheckpoint(Index *index)
{
  IndexCheckpoint *checkpoint;
  int result
    = ALLOCATE(1, IndexCheckpoint, "IndexCheckpoint", &checkpoint);
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
void freeIndexCheckpoint(IndexCheckpoint *checkpoint)
{
  if (checkpoint != NULL) {
    destroyMutex(&checkpoint->mutex);
    FREE(checkpoint);
  }
}

/**********************************************************************/
unsigned int getIndexCheckpointFrequency(IndexCheckpoint *checkpoint)
{
  lockMutex(&checkpoint->mutex);
  unsigned int frequency = checkpoint->frequency;
  unlockMutex(&checkpoint->mutex);
  return frequency;
}

/**********************************************************************/
unsigned int setIndexCheckpointFrequency(IndexCheckpoint *checkpoint,
                                         unsigned int     frequency)
{
  lockMutex(&checkpoint->mutex);
  unsigned int oldFrequency = checkpoint->frequency;
  checkpoint->frequency = frequency;
  unlockMutex(&checkpoint->mutex);
  return oldFrequency;
}

/**********************************************************************/
uint64_t getCheckpointCount(IndexCheckpoint *checkpoint)
{
  return checkpoint->checkpoints;
}

/**********************************************************************/
static IndexCheckpointTriggerValue
getCheckpointAction(IndexCheckpoint *checkpoint,
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
int processCheckpointing(Index        *index,
                         unsigned int  zone,
                         uint64_t      newVirtualChapter)
{
  IndexCheckpoint *checkpoint = index->checkpoint;
  lockMutex(&checkpoint->mutex);

  IndexCheckpointTriggerValue ictv
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
int processChapterWriterCheckpointSaves(Index *index)
{
  IndexCheckpoint *checkpoint = index->checkpoint;

  int result = UDS_SUCCESS;

  lockMutex(&checkpoint->mutex);
  if (checkpoint->state == CHECKPOINT_IN_PROGRESS) {
    result =
      performIndexStateCheckpointChapterSynchronizedSaves(index->state);

    if (result != UDS_SUCCESS) {
      checkpoint->state = CHECKPOINT_ABORTING;
      logInfo("checkpoint failed");
      index->lastCheckpoint = index->prevCheckpoint;
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
static int abortCheckpointing(Index *index, int result)
{
  if (index->checkpoint->state != NOT_CHECKPOINTING) {
    index->checkpoint->state = CHECKPOINT_ABORTING;
    logInfo("checkpoint failed");
    index->lastCheckpoint = index->prevCheckpoint;
  }
  return result;
}

/**********************************************************************/
int finishCheckpointing(Index *index)
{
  IndexCheckpoint *checkpoint = index->checkpoint;

  int result = processChapterWriterCheckpointSaves(index);
  if (result != UDS_SUCCESS) {
    return result;
  }

  lockMutex(&checkpoint->mutex);

  for (unsigned int z = 0; z < index->zoneCount; ++z) {
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
    result = finishIndexStateCheckpoint(index->state);
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
static int doCheckpointStart(Index *index, unsigned int zone)
{
  IndexCheckpoint *checkpoint = index->checkpoint;
  beginSave(index, true, checkpoint->chapter);
  int result = startIndexStateCheckpoint(index->state);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "cannot start index checkpoint");
    index->lastCheckpoint = index->prevCheckpoint;
    unlockMutex(&checkpoint->mutex);
    return result;
  }

  checkpoint->state = CHECKPOINT_IN_PROGRESS;
  checkpoint->zonesBusy = index->zoneCount;

  return doCheckpointProcess(index, zone);
}

/**********************************************************************/
static int doCheckpointProcess(Index *index, unsigned int zone)
{
  IndexCheckpoint *checkpoint = index->checkpoint;
  unlockMutex(&checkpoint->mutex);
  CompletionStatus status = CS_NOT_COMPLETED;
  int result = performIndexStateCheckpointInZone(index->state, zone, &status);
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
      result = finishIndexStateCheckpoint(index->state);
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
static int doCheckpointAbort(Index *index, unsigned int zone)
{
  IndexCheckpoint *checkpoint = index->checkpoint;
  CompletionStatus status = CS_NOT_COMPLETED;
  int result = abortIndexStateCheckpointInZone(index->state, zone, &status);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "cannot abort index checkpoint");
  } else if (status == CS_JUST_COMPLETED) {
    if (--checkpoint->zonesBusy == 0) {
      logInfo("aborted checkpoint");
      result = abortIndexStateCheckpoint(index->state);
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
static int doCheckpointFinish(Index *index, unsigned int zone)
{
  IndexCheckpoint *checkpoint = index->checkpoint;
  CompletionStatus status = CS_NOT_COMPLETED;
  unlockMutex(&checkpoint->mutex);
  int result = finishIndexStateCheckpointInZone(index->state, zone, &status);
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
      result = finishIndexStateCheckpoint(index->state);
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
