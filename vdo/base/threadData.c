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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/threadData.c#2 $
 */

#include "threadData.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "blockAllocator.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "vdoInternal.h"

enum {
  ALLOCATIONS_PER_ZONE = 128,
};

typedef struct {
  VDOCompletion  completion;
  VDO           *vdo;
} WaitCompletion;

/**
 * Convert a generic VDOCompletion to a WaitCompletion.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a WaitCompletion
 **/
static inline WaitCompletion *asWaitCompletion(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(WaitCompletion, completion) == 0);
  assertCompletionType(completion->type, WAIT_FOR_READ_ONLY_MODE_COMPLETION);
  return (WaitCompletion *) completion;
}

/**
 * Convert a generic VDOCompletion to a ThreadData.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a ThreadData
 **/
static ThreadData *asThreadData(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(ThreadData, completion) == 0);
  assertCompletionType(completion->type, READ_ONLY_MODE_COMPLETION);
  return (ThreadData *) completion;
}

/**********************************************************************/
static int initializeThreadData(ThreadData         *threadData,
                                ThreadID            threadID,
                                bool                isReadOnly,
                                const ThreadConfig *threadConfig,
                                PhysicalLayer      *layer)
{
  threadData->threadID             = threadID;
  threadData->isReadOnly           = isReadOnly;
  threadData->mayEnterReadOnlyMode = true;
  threadData->threadConfig         = threadConfig;
  threadData->nextAllocationZone
    = threadID % threadConfig->physicalZoneCount;
  return initializeEnqueueableCompletion(&threadData->completion,
                                         READ_ONLY_MODE_COMPLETION, layer);
}

/**********************************************************************/
int makeThreadDataArray(bool                 isReadOnly,
                        const ThreadConfig  *threadConfig,
                        PhysicalLayer       *layer,
                        ThreadData         **threadsPtr)
{
  ThreadData *threads;
  int result = ALLOCATE(threadConfig->baseThreadCount, ThreadData, __func__,
                        &threads);
  if (result != VDO_SUCCESS) {
    return result;
  }

  for (ThreadCount id = 0; id < threadConfig->baseThreadCount; id++) {
    result = initializeThreadData(&threads[id], id, isReadOnly,
                                  threadConfig, layer);
    if (result != VDO_SUCCESS) {
      freeThreadDataArray(&threads, id);
      return result;
    }
  }

  *threadsPtr = threads;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeThreadDataArray(ThreadData **threadsPtr, ThreadCount count)
{
  ThreadData *threads = *threadsPtr;
  if (threads == NULL) {
    return;
  }

  for (ThreadCount id = 0; id < count; id++) {
    destroyEnqueueable(&threads[id].completion);
  }

  FREE(threads);
  *threadsPtr = NULL;
}

/**********************************************************************/
PhysicalZone *getNextAllocationZone(VDO *vdo, ThreadID threadID)
{
  ThreadData         *threadData   = &vdo->threadData[threadID];
  const ThreadConfig *threadConfig = threadData->threadConfig;
  if (threadConfig->physicalZoneCount > 1) {
    if (threadData->allocationCount < ALLOCATIONS_PER_ZONE) {
      threadData->allocationCount++;
    } else {
      threadData->allocationCount = 1;
      threadData->nextAllocationZone++;
      if (threadData->nextAllocationZone == threadConfig->physicalZoneCount) {
        threadData->nextAllocationZone = 0;
      }
    }
  }

  return vdo->physicalZones[threadData->nextAllocationZone];
}

/**
 * Check whether a thread is entering read-only mode. If so, wait until it
 * isn't. If not, go on to the next thread. If it is the last thread, notify
 * the parent that no threads are entering read-only mode. This callback is
 * registered in waitUntilNotEnteringReadOnlyMode() and also called from
 * finishEnteringReadOnlyMode().
 *
 * @param completion  The wait completion
 **/
static void waitUntilThreadNotEnteringReadOnlyMode(VDOCompletion *completion)
{
  WaitCompletion *waitCompletion = asWaitCompletion(completion);
  VDO *vdo = waitCompletion->vdo;
  ThreadData *threadData = &vdo->threadData[completion->callbackThreadID];

  if (threadData->isEnteringReadOnlyMode
      || (threadData->superBlockAccessState == READING_SUPER_BLOCK)) {
    threadData->superBlockIdleWaiter = completion;
    return;
  }

  threadData->mayEnterReadOnlyMode = false;
  ThreadID nextThread = threadData->threadID + 1;
  if (nextThread == getThreadConfig(vdo)->baseThreadCount) {
    VDOCompletion *parent = completion->parent;
    destroyEnqueueable(completion);
    FREE(completion);
    completeCompletion(parent);
    return;
  }

  launchCallback(completion, waitUntilThreadNotEnteringReadOnlyMode,
                 nextThread);
}

/**********************************************************************/
void waitUntilNotEnteringReadOnlyMode(VDO *vdo, VDOCompletion *parent)
{
  if (vdo->threadData == NULL) {
    finishCompletion(parent, VDO_SUCCESS);
    return;
  }

  WaitCompletion *completion;
  int result = ALLOCATE(1, WaitCompletion, __func__, &completion);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  completion->vdo = vdo;
  result = initializeEnqueueableCompletion(&completion->completion,
                                           WAIT_FOR_READ_ONLY_MODE_COMPLETION,
                                           vdo->layer);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  launchCallbackWithParent(&completion->completion,
                           waitUntilThreadNotEnteringReadOnlyMode, 0, parent);
}

/**
 * Complete the process of entering read only mode.
 *
 * @param completion  The read-only mode completion
 **/
static void finishEnteringReadOnlyMode(VDOCompletion *completion)
{
  ThreadData *threadData             = asThreadData(completion);
  threadData->isEnteringReadOnlyMode = false;
  threadData->superBlockAccessState  = NOT_ACCESSING_SUPER_BLOCK;

  VDOCompletion *waiter = threadData->superBlockIdleWaiter;
  if (waiter != NULL) {
    threadData->superBlockIdleWaiter = NULL;
    waitUntilThreadNotEnteringReadOnlyMode(waiter);
  }
}

/**
 * Inform each thread that the VDO is in read-only mode.
 *
 * @param completion  The read-only mode completion
 **/
static void makeThreadReadOnly(VDOCompletion *completion)
{
  VDO        *vdo        = completion->parent;
  ThreadData *threadData = &vdo->threadData[completion->callbackThreadID];
  threadData->isReadOnly = true;

  // Inform the recovery journal and block allocators that we have entered
  // read-only mode if we are on the correct thread to do so.
  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  if (threadData->threadID == threadConfig->journalThread) {
    notifyRecoveryJournalOfReadOnlyMode(vdo->recoveryJournal);
  }
  for (ZoneCount z = 0; z < threadConfig->physicalZoneCount; z++) {
    if (threadData->threadID == getPhysicalZoneThread(threadConfig, z)) {
      BlockAllocator *allocator = getBlockAllocatorForZone(vdo->depot, z);
      notifyBlockAllocatorOfReadOnlyMode(allocator);
    }
  }

  ThreadID nextThread = threadData->threadID + 1;
  if (nextThread == getThreadConfig(vdo)->baseThreadCount) {
    ThreadData *owner = asThreadData(completion);
    launchCallback(completion, finishEnteringReadOnlyMode, owner->threadID);
    return;
  }

  launchCallback(completion, makeThreadReadOnly, nextThread);
}

/**
 * Proceed to informing each thread that the VDO has entered read-only mode.
 * This callback is registered in setReadOnlyState().
 *
 * @param completion  The read-only mode completion
 **/
static void readOnlyStateSaved(VDOCompletion *completion)
{
  resetCompletion(completion);
  makeThreadReadOnly(completion);
}

/**
 * Handle an error saving the super block. This error handler is registered in
 * setReadOnlyState().
 *
 * @param completion  The read-only mode completion
 **/
static void handleSaveError(VDOCompletion *completion)
{
  logErrorWithStringError(completion->result,
                          "Failed to save super block upon entering "
                          "read-only mode");
  readOnlyStateSaved(completion);
}

/**
 * Actually set the VDO state to read-only and save the read-only state.
 *
 * @param completion      The read-only mode completion
 * @param saveSuperBlock  <code>true</code> if the super block should be
 *                        saved, otherwise the read-only state will be in
 *                        memory only
 **/
static void setReadOnlyState(VDOCompletion *completion, bool saveSuperBlock)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == 0),
                  "setReadOnlyState() called on thread 0");
  ThreadData *threadData = asThreadData(completion);
  VDO        *vdo        = completion->parent;
  if (vdo->state == VDO_READ_ONLY_MODE) {
    // We've already entered read-only mode.
    launchCallback(completion, finishEnteringReadOnlyMode,
                   threadData->threadID);
    return;
  }

  // Note: This message must be recognizable by Permabit::VDODeviceBase.
  logErrorWithStringError(threadData->readOnlyError,
                            "Unrecoverable error, entering read-only mode");
  vdo->state = VDO_READ_ONLY_MODE;

  ThreadData *adminThreadData = &vdo->threadData[0];
  if (adminThreadData->superBlockAccessState != NOT_ACCESSING_SUPER_BLOCK) {
    adminThreadData->readOnlyModeWaiter = completion;
    return;
  }

  if (!saveSuperBlock) {
    makeThreadReadOnly(completion);
    return;
  }

  adminThreadData->superBlockAccessState = WRITING_SUPER_BLOCK;
  completion->callback                   = readOnlyStateSaved;
  completion->errorHandler               = handleSaveError;
  saveVDOComponentsAsync(vdo, completion);
}

/**
 * Set the VDO's state to read-only in memory only. This callback is registered
 * in makeVDOReadOnly().
 *
 * @param completion  The read-only mode completion
 **/
static void setReadOnlyStateNoSave(VDOCompletion *completion)
{
  setReadOnlyState(completion, false);
}

/**
 * Set the VDO's state to read-only in memory and on disk. This callback is
 * registered in makeVDOReadOnly().
 *
 * @param completion  The read-only mode completion
 **/
static void setReadOnlyStateAndSave(VDOCompletion *completion)
{
  setReadOnlyState(completion, true);
}

/**********************************************************************/
void makeVDOReadOnly(VDO *vdo, int errorCode, bool saveSuperBlock)
{
  ThreadData *threadData = getThreadData(vdo);
  if (threadData->isReadOnly) {
    return;
  }

  threadData->isReadOnly = true;
  if (threadData->mayEnterReadOnlyMode == false) {
    return;
  }

  threadData->isEnteringReadOnlyMode = true;
  threadData->readOnlyError          = errorCode;
  if (saveSuperBlock) {
    launchCallbackWithParent(&threadData->completion, setReadOnlyStateAndSave,
                             0, vdo);
  } else {
    launchCallbackWithParent(&threadData->completion, setReadOnlyStateNoSave,
                             0, vdo);
  }
}
