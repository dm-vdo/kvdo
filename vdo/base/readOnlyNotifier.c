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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/readOnlyNotifier.c#7 $
 */

#include "readOnlyNotifier.h"

#include "atomic.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "physicalLayer.h"
#include "threadConfig.h"

/**
 * A read_only_notifier has a single completion which is used to perform
 * read-only notifications, however, enterReadOnlyMode() may be called from any
 * base thread. A pair of atomic fields are used to control the read-only mode
 * entry process. The first field holds the read-only error. The second is the
 * state field, which may hold any of the four special values enumerated here.
 *
 * When enterReadOnlyMode() is called from some base thread, a compare-and-swap
 * is done on the readOnlyError, setting it to the supplied error if the value
 * was VDO_SUCCESS. If this fails, some other thread has already intiated
 * read-only entry or scheduled a pending entry, so the call exits. Otherwise,
 * a compare-and-swap is done on the state, setting it to NOTIFYING if the
 * value was MAY_NOTIFY. If this succeeds, the caller initiates the
 * notification. If this failed due to notifications being disallowed, the
 * notifier will be in the MAY_NOT_NOTIFY state but readOnlyError will not be
 * VDO_SUCCESS. This configuration will indicate to allowReadOnlyModeEntry()
 * that there is a pending notification to perform.
 **/
enum {
  /** Notifications are allowed but not in progress */
  MAY_NOTIFY = 0,
  /** A notification is in progress */
  NOTIFYING,
  /** Notifications are not allowed */
  MAY_NOT_NOTIFY,
  /** A notification has completed */
  NOTIFIED,
};

/**
 * An object to be notified when the VDO enters read-only mode
 **/
struct read_only_listener {
  /** The listener */
  void                          *listener;
  /** The method to call to notify the listener */
  ReadOnlyNotification          *notify;
  /** A pointer to the next listener */
  struct read_only_listener     *next;
};

/**
 * Data associated with each base code thread.
 **/
struct thread_data {
  /**
   * Each thread maintains its own notion of whether the VDO is read-only so
   * that the read-only state can be checked from any base thread without
   * worrying about synchronization or thread safety. This does mean that
   * knowledge of the VDO going read-only does not occur simultaneously across
   * the VDO's threads, but that does not seem to cause any problems.
   */
  bool              isReadOnly;
  /**
   * A list of objects waiting to be notified on this thread that the VDO has
   * entered read-only mode.
   **/
  struct read_only_listener *listeners;
};

struct read_only_notifier {
  /** The completion for entering read-only mode */
  struct vdo_completion  completion;
  /** A completion waiting for notifications to be drained or enabled */
  struct vdo_completion *waiter;
  /** The code of the error which put the VDO into read-only mode */
  Atomic32               readOnlyError;
  /** The current state of the notifier (values described above) */
  Atomic32               state;
  /** The thread config of the VDO */
  const ThreadConfig    *threadConfig;
  /** The array of per-thread data */
  struct thread_data     threadData[];
};

/**
 * Convert a generic vdo_completion to a read_only_notifier.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a read_only_notifier
 **/
static inline struct read_only_notifier *
asNotifier(struct vdo_completion *completion)
{
  STATIC_ASSERT(offsetof(struct read_only_notifier, completion) == 0);
  assertCompletionType(completion->type, READ_ONLY_MODE_COMPLETION);
  return (struct read_only_notifier *) completion;
}

/**********************************************************************/
int makeReadOnlyNotifier(bool                        isReadOnly,
                         const ThreadConfig         *threadConfig,
                         PhysicalLayer              *layer,
                         struct read_only_notifier **notifierPtr)
{
  struct read_only_notifier *notifier;
  int result = ALLOCATE_EXTENDED(struct read_only_notifier,
                                 threadConfig->baseThreadCount,
                                 struct thread_data,
                                 __func__, &notifier);
  if (result != VDO_SUCCESS) {
    return result;
  }

  notifier->threadConfig = threadConfig;
  if (isReadOnly) {
    atomicStore32(&notifier->readOnlyError, (uint32_t) VDO_READ_ONLY);
    atomicStore32(&notifier->state, NOTIFIED);
  } else {
    atomicStore32(&notifier->state, MAY_NOTIFY);
  }
  result = initializeEnqueueableCompletion(&notifier->completion,
                                           READ_ONLY_MODE_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    freeReadOnlyNotifier(&notifier);
    return result;
  }

  for (ThreadCount id = 0; id < threadConfig->baseThreadCount; id++) {
    notifier->threadData[id].isReadOnly = isReadOnly;
  }

  *notifierPtr = notifier;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeReadOnlyNotifier(struct read_only_notifier **notifierPtr)
{
  struct read_only_notifier *notifier = *notifierPtr;
  if (notifier == NULL) {
    return;
  }

  for (ThreadCount id = 0; id < notifier->threadConfig->baseThreadCount;
       id++) {
    struct thread_data        *threadData = &notifier->threadData[id];
    struct read_only_listener *listener   = threadData->listeners;
    while (listener != NULL) {
      struct read_only_listener *toFree = listener;
      listener = listener->next;
      FREE(toFree);
    }
  }

  destroyEnqueueable(&notifier->completion);
  FREE(notifier);
  *notifierPtr = NULL;
}

/**
 * Check that a function was called on the admin thread.
 *
 * @param notifier  The notifier
 * @param caller    The name of the function (for logging)
 **/
static void assertOnAdminThread(struct read_only_notifier *notifier,
                                const char                *caller)
{
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((getAdminThread(notifier->threadConfig) == threadID),
                  "%s called on admin thread", caller);
}


/**********************************************************************/
void waitUntilNotEnteringReadOnlyMode(struct read_only_notifier *notifier,
                                      struct vdo_completion     *parent)
{
  if (notifier == NULL) {
    finishCompletion(parent, VDO_SUCCESS);
    return;
  }

  assertOnAdminThread(notifier, __func__);
  if (notifier->waiter != NULL) {
    finishCompletion(parent, VDO_COMPONENT_BUSY);
    return;
  }

  uint32_t state = atomicLoad32(&notifier->state);
  if ((state == MAY_NOT_NOTIFY) || (state == NOTIFIED)) {
    // Notifications are already done or disallowed.
    completeCompletion(parent);
    return;
  }

  if (compareAndSwap32(&notifier->state, MAY_NOTIFY, MAY_NOT_NOTIFY)) {
    // A notification was not in progress, and now they are disallowed.
    completeCompletion(parent);
    return;
  }

  /*
   * A notification is in progress, so wait for it to finish. There is no race
   * here since the notification can't finish while the admin thread is in this
   * method.
   */
  notifier->waiter = parent;
}

/**
 * Complete the process of entering read only mode.
 *
 * @param completion  The read-only mode completion
 **/
static void finishEnteringReadOnlyMode(struct vdo_completion *completion)
{
  struct read_only_notifier *notifier = asNotifier(completion);
  assertOnAdminThread(notifier, __func__);
  atomicStore32(&notifier->state, NOTIFIED);

  struct vdo_completion *waiter = notifier->waiter;
  if (waiter != NULL) {
    notifier->waiter = NULL;
    finishCompletion(waiter, completion->result);
  }
}

/**
 * Inform each thread that the VDO is in read-only mode.
 *
 * @param completion  The read-only mode completion
 **/
static void makeThreadReadOnly(struct vdo_completion *completion)
{
  ThreadID                   threadID   = completion->callbackThreadID;
  struct read_only_notifier *notifier   = asNotifier(completion);
  struct read_only_listener *listener   = completion->parent;
  if (listener == NULL) {
    // This is the first call on this thread
    struct thread_data *threadData = &notifier->threadData[threadID];
    threadData->isReadOnly         = true;
    listener                       = threadData->listeners;
    if (threadID == 0) {
      // Note: This message must be recognizable by Permabit::UserMachine.
      logErrorWithStringError((int) atomicLoad32(&notifier->readOnlyError),
                              "Unrecoverable error, entering read-only mode");
    }
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

/**********************************************************************/
void allowReadOnlyModeEntry(struct read_only_notifier *notifier,
                            struct vdo_completion     *parent)
{
  assertOnAdminThread(notifier, __func__);
  if (notifier->waiter != NULL) {
    finishCompletion(parent, VDO_COMPONENT_BUSY);
    return;
  }

   if (!compareAndSwap32(&notifier->state, MAY_NOT_NOTIFY, MAY_NOTIFY)) {
    // Notifications were already allowed or complete
    completeCompletion(parent);
    return;
  }

  if ((int) atomicLoad32(&notifier->readOnlyError) == VDO_SUCCESS) {
    // We're done
    completeCompletion(parent);
    return;
  }

  // There may have been a pending notification
  if (!compareAndSwap32(&notifier->state, MAY_NOTIFY, NOTIFYING)) {
    /*
     * There wasn't, the error check raced with a thread calling
     * enterReadOnlyMode() after we set the state to MAY_NOTIFY. It has already
     * started the notification.
     */
    completeCompletion(parent);
    return;
  }

  // Do the pending notification.
  notifier->waiter = parent;
  makeThreadReadOnly(&notifier->completion);
}

/**********************************************************************/
void enterReadOnlyMode(struct read_only_notifier *notifier, int errorCode)
{
  struct thread_data *threadData = &notifier->threadData[getCallbackThreadID()];
  if (threadData->isReadOnly) {
    // This thread has already gone read-only.
    return;
  }

  // Record for this thread that the VDO is read-only.
  threadData->isReadOnly = true;

  if (!compareAndSwap32(&notifier->readOnlyError, (uint32_t) VDO_SUCCESS,
                        (uint32_t) errorCode)) {
    // The notifier is already aware of a read-only error
    return;
  }

  if (compareAndSwap32(&notifier->state, MAY_NOTIFY, NOTIFYING)) {
    // Initiate a notification starting on the lowest numbered thread.
    launchCallback(&notifier->completion, makeThreadReadOnly, 0);
  }
}

/**********************************************************************/
bool isReadOnly(struct read_only_notifier *notifier)
{
  return notifier->threadData[getCallbackThreadID()].isReadOnly;
}

/**********************************************************************/
int registerReadOnlyListener(struct read_only_notifier *notifier,
                             void                      *listener,
                             ReadOnlyNotification      *notification,
                             ThreadID                   threadID)
{
  struct read_only_listener *readOnlyListener;
  int result = ALLOCATE(1, struct read_only_listener,
                        __func__, &readOnlyListener);
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct thread_data *threadData = &notifier->threadData[threadID];
  *readOnlyListener = (struct read_only_listener) {
    .listener = listener,
    .notify   = notification,
    .next     = threadData->listeners,
  };

  threadData->listeners = readOnlyListener;
  return VDO_SUCCESS;
}
