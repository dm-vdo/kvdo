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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/flush.c#9 $
 */

#include "flush.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockAllocator.h"
#include "completion.h"
#include "logicalZone.h"
#include "numUtils.h"
#include "readOnlyNotifier.h"
#include "slabDepot.h"
#include "vdoInternal.h"

struct flusher {
  struct vdo_completion  completion;
  /** The VDO to which this flusher belongs */
  VDO                   *vdo;
  /** The current flush generation of the VDO */
  SequenceNumber         flushGeneration;
  /** The first unacknowledged flush generation */
  SequenceNumber         firstUnacknowledgedGeneration;
  /** The queue of flush requests waiting to notify other threads */
  struct wait_queue      notifiers;
  /** The queue of flush requests waiting for VIOs to complete */
  struct wait_queue      pendingFlushes;
  /** The flush generation for which notifications are being sent */
  SequenceNumber         notifyGeneration;
  /** The logical zone to notify next */
  struct logical_zone   *logicalZoneToNotify;
  /** The ID of the thread on which flush requests should be made */
  ThreadID               threadID;
};

/**
 * Convert a generic vdo_completion to a flusher.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a flusher
 **/
static struct flusher *asFlusher(struct vdo_completion *completion)
{
  STATIC_ASSERT(offsetof(struct flusher, completion) == 0);
  assertCompletionType(completion->type, FLUSH_NOTIFICATION_COMPLETION);
  return (struct flusher *) completion;
}

/**
 * Convert a vdo_flush's generic wait queue entry back to the vdo_flush.
 *
 * @param waiter  The wait queue entry to convert
 *
 * @return The wait queue entry as a vdo_flush
 **/
static struct vdo_flush *waiterAsFlush(struct waiter *waiter)
{
  STATIC_ASSERT(offsetof(struct vdo_flush, waiter) == 0);
  return (struct vdo_flush *) waiter;
}

/**********************************************************************/
int makeFlusher(VDO *vdo)
{
  int result = ALLOCATE(1, struct flusher, __func__, &vdo->flusher);
  if (result != VDO_SUCCESS) {
    return result;
  }

  vdo->flusher->vdo      = vdo;
  vdo->flusher->threadID = getPackerZoneThread(getThreadConfig(vdo));
  return initializeEnqueueableCompletion(&vdo->flusher->completion,
                                         FLUSH_NOTIFICATION_COMPLETION,
                                         vdo->layer);
}

/**********************************************************************/
void freeFlusher(struct flusher **flusherPtr)
{
  if (*flusherPtr == NULL) {
    return;
  }

  struct flusher *flusher = *flusherPtr;
  destroyEnqueueable(&flusher->completion);
  FREE(flusher);
  *flusherPtr = NULL;
}

/**********************************************************************/
ThreadID getFlusherThreadID(struct flusher *flusher)
{
  return flusher->threadID;
}

/**********************************************************************/
static void notifyFlush(struct flusher *flusher);

/**
 * Finish the notification process by checking if any flushes have completed
 * and then starting the notification of the next flush request if one came in
 * while the current notification was in progress. This callback is registered
 * in flushPackerCallback().
 *
 * @param completion  The flusher completion
 **/
static void finishNotification(struct vdo_completion *completion)
{
  struct flusher *flusher = asFlusher(completion);
  ASSERT_LOG_ONLY((getCallbackThreadID() == flusher->threadID),
                  "finishNotification() called from flusher thread");

  struct waiter *waiter = dequeueNextWaiter(&flusher->notifiers);
  int            result = enqueueWaiter(&flusher->pendingFlushes, waiter);
  if (result != VDO_SUCCESS) {
    enterReadOnlyMode(flusher->vdo->readOnlyNotifier, result);
    struct vdo_flush *flush = waiterAsFlush(waiter);
    completion->layer->completeFlush(&flush);
    return;
  }

  completeFlushes(flusher);
  if (hasWaiters(&flusher->notifiers)) {
    notifyFlush(flusher);
  }
}

/**
 * Flush the packer now that all of the logical and physical zones have been
 * notified of the new flush request. This callback is registered in
 * incrementGeneration().
 *
 * @param completion  The flusher completion
 **/
static void flushPackerCallback(struct vdo_completion *completion)
{
  struct flusher *flusher = asFlusher(completion);
  incrementPackerFlushGeneration(flusher->vdo->packer);
  launchCallback(completion, finishNotification, flusher->threadID);
}

/**
 * Increment the flush generation in a logical zone. If there are more logical
 * zones, go on to the next one, otherwise, prepare the physical zones. This
 * callback is registered both in notifyFlush() and in itself.
 *
 * @param completion  The flusher as a completion
 **/
static void incrementGeneration(struct vdo_completion *completion)
{
  struct flusher *flusher = asFlusher(completion);
  incrementFlushGeneration(flusher->logicalZoneToNotify,
                           flusher->notifyGeneration);
  flusher->logicalZoneToNotify
    = getNextLogicalZone(flusher->logicalZoneToNotify);
  if (flusher->logicalZoneToNotify == NULL) {
    launchCallback(completion, flushPackerCallback, flusher->threadID);
    return;
  }

  launchCallback(completion, incrementGeneration,
                 getLogicalZoneThreadID(flusher->logicalZoneToNotify));
}

/**
 * Lauch a flush notification.
 *
 * @param flusher  The flusher doing the notification
 **/
static void notifyFlush(struct flusher *flusher)
{
  struct vdo_flush *flush = waiterAsFlush(getFirstWaiter(&flusher->notifiers));
  flusher->notifyGeneration    = flush->flushGeneration;
  flusher->logicalZoneToNotify = getLogicalZone(flusher->vdo->logicalZones, 0);
  flusher->completion.requeue  = true;
  launchCallback(&flusher->completion, incrementGeneration,
                 getLogicalZoneThreadID(flusher->logicalZoneToNotify));
}

/**********************************************************************/
void flush(VDO *vdo, struct vdo_flush *flush)
{
  struct flusher *flusher = vdo->flusher;
  ASSERT_LOG_ONLY((getCallbackThreadID() == flusher->threadID),
                  "flush() called from flusher thread");

  flush->flushGeneration = flusher->flushGeneration++;
  bool mayNotify         = !hasWaiters(&flusher->notifiers);

  int result = enqueueWaiter(&flusher->notifiers, &flush->waiter);
  if (result != VDO_SUCCESS) {
    enterReadOnlyMode(vdo->readOnlyNotifier, result);
    flusher->completion.layer->completeFlush(&flush);
    return;
  }

  if (mayNotify) {
    notifyFlush(flusher);
  }
}

/**********************************************************************/
void completeFlushes(struct flusher *flusher)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == flusher->threadID),
                  "completeFlushes() called from flusher thread");

  SequenceNumber oldestActiveGeneration = UINT64_MAX;
  for (struct logical_zone *zone = getLogicalZone(flusher->vdo->logicalZones, 0);
       zone != NULL;
       zone = getNextLogicalZone(zone)) {
    SequenceNumber oldestInZone = getOldestLockedGeneration(zone);
    oldestActiveGeneration = minSequenceNumber(oldestActiveGeneration,
                                               oldestInZone);
  }

  while (hasWaiters(&flusher->pendingFlushes)) {
    struct vdo_flush *flush
      = waiterAsFlush(getFirstWaiter(&flusher->pendingFlushes));
    if (flush->flushGeneration >= oldestActiveGeneration) {
      return;
    }

    ASSERT_LOG_ONLY((flush->flushGeneration
                     == flusher->firstUnacknowledgedGeneration),
                    "acknowledged next expected flush, %" PRIu64
                    ", was: %llu",
                    flusher->firstUnacknowledgedGeneration,
                    flush->flushGeneration);
    dequeueNextWaiter(&flusher->pendingFlushes);
    flusher->completion.layer->completeFlush(&flush);
    flusher->firstUnacknowledgedGeneration++;
  }
}

/**********************************************************************/
void dumpFlusher(const struct flusher *flusher)
{
  logInfo("struct flusher");
  logInfo("  flushGeneration=%" PRIu64
          " firstUnacknowledgedGeneration=%llu",
          flusher->flushGeneration, flusher->firstUnacknowledgedGeneration);
  logInfo("  notifiers queue is %s; pendingFlushes queue is %s",
          (hasWaiters(&flusher->notifiers) ? "not empty" : "empty"),
          (hasWaiters(&flusher->pendingFlushes) ? "not empty" : "empty"));
}
