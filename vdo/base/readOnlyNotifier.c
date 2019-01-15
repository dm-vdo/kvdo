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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/readOnlyNotifier.c#2 $
 */

#include "readOnlyNotifier.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "physicalLayer.h"
#include "threadConfig.h"

/**
 * An object to be notified when the VDO enters read-only mode
 **/
typedef struct readOnlyListener ReadOnlyListener;

struct readOnlyListener {
  /** The listener */
  void                 *listener;
  /** The method to call to notifiy the listener */
  ReadOnlyNotification *notify;
  /** A pointer to the next listener */
  ReadOnlyListener     *next;
};

/**
 * Data associated with each base code thread.
 **/
typedef struct threadData {
  /** The completion for entering read-only mode */
  VDOCompletion          completion;
  /** The ReadOnlyNotifier to which this structure belongs */
  ReadOnlyNotifier      *notifier;
  /** The thread this represents */
  ThreadID               threadID;
  /** Whether this thread is in read-only mode */
  bool                   isReadOnly;
  /** Whether this thread is entering read-only mode */
  bool                   isEnteringReadOnlyMode;
  /** Whether this thread may enter read-only mode */
  bool                   mayEnterReadOnlyMode;
  /** The error code for entering read-only mode */
  int                    readOnlyError;
  /** A completion to notify when this thread is not entering read-only mode */
  VDOCompletion         *superBlockIdleWaiter;
  /** A completion which is waiting to enter read-only mode */
  VDOCompletion         *readOnlyModeWaiter;
  /**
   * A list of objects waiting to be notified on this thread that the VDO has
   * entered read-only mode.
   **/
  ReadOnlyListener      *listeners;
} ThreadData;

struct readOnlyNotifier {
  /** The completion for waiting until not entering read-only mode */
  VDOCompletion       completion;
  /** Whether the listener has already started a notification */
  bool                isReadOnly;
  /** The thread config of the VDO */
  const ThreadConfig *threadConfig;
  /** The ID of the admin thread */
  ThreadID            adminThreadID;
  /** The array of per-thread data */
  ThreadData          threadData[];
};

/**
 * Convert a generic VDOCompletion to a ReadOnlyNotifier.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a ReadOnlyNotifier
 **/
static inline ReadOnlyNotifier *asNotifier(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(ReadOnlyNotifier, completion) == 0);
  assertCompletionType(completion->type, WAIT_FOR_READ_ONLY_MODE_COMPLETION);
  return (ReadOnlyNotifier *) completion;
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
int makeReadOnlyNotifier(bool                 isReadOnly,
                         const ThreadConfig  *threadConfig,
                         PhysicalLayer       *layer,
                         ReadOnlyNotifier   **notifierPtr)
{
  ReadOnlyNotifier *notifier;
  int result = ALLOCATE_EXTENDED(ReadOnlyNotifier,
                                 threadConfig->baseThreadCount, ThreadData,
                                 __func__, &notifier);
  if (result != VDO_SUCCESS) {
    return result;
  }

  notifier->isReadOnly    = isReadOnly;
  notifier->threadConfig  = threadConfig;

  result = initializeEnqueueableCompletion(&notifier->completion,
                                           WAIT_FOR_READ_ONLY_MODE_COMPLETION,
                                           layer);
  if (result != VDO_SUCCESS) {
    freeReadOnlyNotifier(&notifier);
    return result;
  }

  for (ThreadCount id = 0; id < threadConfig->baseThreadCount; id++) {
    notifier->threadData[id] = (ThreadData) {
      .notifier             = notifier,
      .threadID             = id,
      .isReadOnly           = isReadOnly,
      .mayEnterReadOnlyMode = true,
    };
    result
      = initializeEnqueueableCompletion(&notifier->threadData[id].completion,
                                        READ_ONLY_MODE_COMPLETION, layer);
    if (result != VDO_SUCCESS) {
      freeReadOnlyNotifier(&notifier);
      return result;
    }
  }

  *notifierPtr = notifier;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeReadOnlyNotifier(ReadOnlyNotifier **notifierPtr)
{
  ReadOnlyNotifier *notifier = *notifierPtr;
  if (notifier == NULL) {
    return;
  }

  for (ThreadCount id = 0; id < notifier->threadConfig->baseThreadCount;
       id++) {
    ThreadData *threadData = &notifier->threadData[id];
    destroyEnqueueable(&threadData->completion);
    ReadOnlyListener *listener = threadData->listeners;
    while (listener != NULL) {
      ReadOnlyListener *toFree = listener;
      listener = listener->next;
      FREE(toFree);
    }
  }

  destroyEnqueueable(&notifier->completion);
  FREE(notifier);
  *notifierPtr = NULL;
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
  ReadOnlyNotifier *notifier   = asNotifier(completion);
  ThreadID          threadID   = completion->callbackThreadID;
  ThreadData       *threadData = &notifier->threadData[threadID];

  if (threadData->isEnteringReadOnlyMode) {
    threadData->superBlockIdleWaiter = completion;
    return;
  }

  threadData->mayEnterReadOnlyMode = false;
  if (++threadID == notifier->threadConfig->baseThreadCount) {
    VDOCompletion *parent = completion->parent;
    completion->parent    = NULL;
    completeCompletion(parent);
    return;
  }

  launchCallback(completion, waitUntilThreadNotEnteringReadOnlyMode, threadID);
}

/**********************************************************************/
void waitUntilNotEnteringReadOnlyMode(ReadOnlyNotifier *notifier,
                                      VDOCompletion    *parent)
{
  if (notifier == NULL) {
    finishCompletion(parent, VDO_SUCCESS);
    return;
  }

  if (notifier->completion.parent != NULL) {
    finishCompletion(parent, VDO_COMPONENT_BUSY);
    return;
  }

  ThreadID adminThreadID = getAdminThread(notifier->threadConfig);
  ASSERT_LOG_ONLY((getCallbackThreadID() == adminThreadID),
                  "waitUntilNotEnteringReadOnlyMode() called on admin thread");
  launchCallbackWithParent(&notifier->completion,
                           waitUntilThreadNotEnteringReadOnlyMode,
                           adminThreadID, parent);
}

/**
 * Complete the process of entering read only mode.
 *
 * @param completion  The read-only mode completion
 **/
static void finishEnteringReadOnlyMode(VDOCompletion *completion)
{
  ThreadData *threadData = asThreadData(completion);
  threadData->isEnteringReadOnlyMode = false;

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
  ThreadID          threadID   = completion->callbackThreadID;
  ReadOnlyNotifier *notifier   = asThreadData(completion)->notifier;
  ThreadData       *threadData = &notifier->threadData[threadID];
  ReadOnlyListener *listener   = completion->parent;
  if (listener == NULL) {
    // This is the first call on this thread
    threadData->isReadOnly = true;
    listener = threadData->listeners;
  } else {
    // We've just finished notifying a listener
    listener = listener->next;
  }

  if (listener != NULL) {
    // We have a listener to notify
    prepareCompletion(completion, makeThreadReadOnly, makeThreadReadOnly,
                      threadID, listener);
    listener->notify(listener->listener, completion);
    return;
  }

  // We're done with this thread
  if (++threadID >= notifier->threadConfig->baseThreadCount) {
    // There are no more threads
    prepareCompletion(completion, finishEnteringReadOnlyMode,
                      finishEnteringReadOnlyMode,
                      getAdminThread(notifier->threadConfig), NULL);
  } else {
    prepareCompletion(completion, makeThreadReadOnly, makeThreadReadOnly,
                      threadID, NULL);
  }

  invokeCallback(completion);
}

/**
 * Start a read-only notification if one has not already been done.
 *
 * @param completion      The read-only mode completion
 **/
static void startNotification(VDOCompletion *completion)
{
  ThreadData       *threadData    = asThreadData(completion);
  ReadOnlyNotifier *notifier      = threadData->notifier;
  ThreadID          adminThreadID = getAdminThread(notifier->threadConfig);
  ASSERT_LOG_ONLY((getCallbackThreadID() == adminThreadID),
                  "startNotification() called on admin thread");

  if (notifier->isReadOnly) {
    // We've already entered read-only mode.
    launchCallback(completion, finishEnteringReadOnlyMode,
                   threadData->threadID);
    return;
  }

  notifier->isReadOnly = true;

  // Note: This message must be recognizable by Permabit::VDODeviceBase.
  logErrorWithStringError(threadData->readOnlyError,
                          "Unrecoverable error, entering read-only mode");
  makeThreadReadOnly(completion);
}

/**********************************************************************/
void enterReadOnlyMode(ReadOnlyNotifier *notifier, int errorCode)
{
  ThreadData *threadData = &notifier->threadData[getCallbackThreadID()];
  if (threadData->isReadOnly) {
    return;
  }

  threadData->isReadOnly = true;
  if (threadData->mayEnterReadOnlyMode == false) {
    return;
  }

  threadData->isEnteringReadOnlyMode = true;
  threadData->readOnlyError          = errorCode;
  launchCallback(&threadData->completion, startNotification,
                 getAdminThread(notifier->threadConfig));
}

/**********************************************************************/
bool isReadOnly(ReadOnlyNotifier *notifier)
{
  return notifier->threadData[getCallbackThreadID()].isReadOnly;
}

/**********************************************************************/
int registerReadOnlyListener(ReadOnlyNotifier     *notifier,
                             void                 *listener,
                             ReadOnlyNotification *notification,
                             ThreadID              threadID)
{
  ReadOnlyListener *readOnlyListener;
  int result = ALLOCATE(1, ReadOnlyListener, __func__, &readOnlyListener);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ThreadData *threadData = &notifier->threadData[threadID];
  *readOnlyListener = (ReadOnlyListener) {
    .listener = listener,
    .notify   = notification,
    .next     = threadData->listeners,
  };

  threadData->listeners = readOnlyListener;
  return VDO_SUCCESS;
}
