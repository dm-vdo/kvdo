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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabScrubber.c#3 $
 */

#include "slabScrubberInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminState.h"
#include "blockAllocator.h"
#include "readOnlyNotifier.h"
#include "slabRebuild.h"

/**********************************************************************/
int makeSlabScrubber(PhysicalLayer     *layer,
                     BlockCount         slabJournalSize,
                     ReadOnlyNotifier  *readOnlyNotifier,
                     SlabScrubber     **scrubberPtr)
{
  SlabScrubber *scrubber;
  int result = ALLOCATE(1, SlabScrubber, __func__, &scrubber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeSlabRebuildCompletion(layer, slabJournalSize,
                                     &scrubber->slabRebuildCompletion);
  if (result != VDO_SUCCESS) {
    freeSlabScrubber(&scrubber);
    return result;
  }

  initializeCompletion(&scrubber->completion, SLAB_SCRUBBER_COMPLETION, layer);
  initializeRing(&scrubber->highPrioritySlabs);
  initializeRing(&scrubber->slabs);
  scrubber->readOnlyNotifier = readOnlyNotifier;
  scrubber->adminState.state = ADMIN_STATE_SUSPENDED;
  *scrubberPtr               = scrubber;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeSlabScrubber(SlabScrubber **scrubberPtr)
{
  if (*scrubberPtr == NULL) {
    return;
  }

  SlabScrubber *scrubber = *scrubberPtr;
  freeSlabRebuildCompletion(&scrubber->slabRebuildCompletion);
  FREE(scrubber);
  *scrubberPtr = NULL;
}

/**
 * Get the next slab to scrub.
 *
 * @param scrubber  The slab scrubber
 *
 * @return The next slab to scrub or <code>NULL</code> if there are none
 **/
static Slab *getNextSlab(SlabScrubber *scrubber)
{
  if (!isRingEmpty(&scrubber->highPrioritySlabs)) {
    return slabFromRingNode(scrubber->highPrioritySlabs.next);
  }

  if (!isRingEmpty(&scrubber->slabs)) {
    return slabFromRingNode(scrubber->slabs.next);
  }

  return NULL;
}

/**********************************************************************/
bool hasSlabsToScrub(SlabScrubber *scrubber)
{
  return (getNextSlab(scrubber) != NULL);
}

/**********************************************************************/
SlabCount getScrubberSlabCount(const SlabScrubber *scrubber)
{
  return relaxedLoad64(&scrubber->slabCount);
}

/**********************************************************************/
void registerSlabForScrubbing(SlabScrubber *scrubber,
                              Slab         *slab,
                              bool          highPriority)
{
  ASSERT_LOG_ONLY((slab->status != SLAB_REBUILT),
                  "slab to be scrubbed is unrecovered");

  if (slab->status != SLAB_REQUIRES_SCRUBBING) {
    return;
  }

  unspliceRingNode(&slab->ringNode);
  if (!slab->wasQueuedForScrubbing) {
    relaxedAdd64(&scrubber->slabCount, 1);
    slab->wasQueuedForScrubbing = true;
  }

  if (highPriority) {
    slab->status = SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING;
    pushRingNode(&scrubber->highPrioritySlabs, &slab->ringNode);
    return;
  }

  pushRingNode(&scrubber->slabs, &slab->ringNode);
}

/**
 * Stop scrubbing, either because there are no more slabs to scrub or because
 * there's been an error.
 *
 * @param scrubber  The scrubber
 **/
static void finishScrubbing(SlabScrubber *scrubber)
{
  if (!hasSlabsToScrub(scrubber)) {
    freeSlabRebuildCompletion(&scrubber->slabRebuildCompletion);
  }

  // Inform whoever is waiting that scrubbing has completed.
  completeCompletion(&scrubber->completion);

  bool notify = hasWaiters(&scrubber->waiters);

  // Note that the scrubber has stopped, and inform anyone who might be waiting
  // for that to happen.
  if (!finishDraining(&scrubber->adminState)) {
    scrubber->adminState.state = ADMIN_STATE_SUSPENDED;
  }

  /*
   * We can't notify waiters until after we've finished draining or they'll
   * just requeue. Fortunately if there were waiters, we can't have been freed
   * yet.
   */
  if (notify) {
    notifyAllWaiters(&scrubber->waiters, NULL, NULL);
  }
}

/**
 * Scrub the next slab if there is one.
 *
 * @param scrubber  The scrubber
 **/
static void scrubNextSlab(SlabScrubber *scrubber)
{
  // Note: this notify call is always safe only because scrubbing can only
  // be started when the VDO is quiescent.
  notifyAllWaiters(&scrubber->waiters, NULL, NULL);
  if (isReadOnly(scrubber->readOnlyNotifier)) {
    setCompletionResult(&scrubber->completion, VDO_READ_ONLY);
    finishScrubbing(scrubber);
    return;
  }

  Slab *slab = getNextSlab(scrubber);
  if ((slab == NULL)
      || (scrubber->highPriorityOnly
          && isRingEmpty(&scrubber->highPrioritySlabs))) {
    scrubber->highPriorityOnly = false;
    finishScrubbing(scrubber);
    return;
  }

  if (finishDraining(&scrubber->adminState)) {
    return;
  }

  unspliceRingNode(&slab->ringNode);
  resetCompletion(scrubber->slabRebuildCompletion);
  scrubSlab(slab, scrubber->slabRebuildCompletion);
}

/**
 * Notify the scrubber that a slab has been scrubbed. This callback is
 * registered in scrubSlabs().
 *
 * @param completion  The slab rebuild completion
 **/
static void slabScrubbed(VDOCompletion *completion)
{
  SlabScrubber *scrubber = completion->parent;
  relaxedAdd64(&scrubber->slabCount, -1);
  scrubNextSlab(scrubber);
}

/**
 * Handle errors while rebuilding a slab.
 *
 * @param completion  The slab rebuild completion
 **/
static void handleScrubberError(VDOCompletion *completion)
{
  SlabScrubber *scrubber = completion->parent;
  enterReadOnlyMode(scrubber->readOnlyNotifier, completion->result);
  setCompletionResult(&scrubber->completion, completion->result);
  scrubNextSlab(scrubber);
}

/**********************************************************************/
void scrubSlabs(SlabScrubber *scrubber,
                void         *parent,
                VDOAction    *callback,
                VDOAction    *errorHandler)
{
  resumeIfQuiescent(&scrubber->adminState);
  ThreadID threadID = getCallbackThreadID();
  prepareCompletion(&scrubber->completion, callback, errorHandler, threadID,
                    parent);
  if (!hasSlabsToScrub(scrubber)) {
    finishScrubbing(scrubber);
    return;
  }

  prepareCompletion(scrubber->slabRebuildCompletion, slabScrubbed,
                    handleScrubberError, threadID, scrubber);
  scrubNextSlab(scrubber);
}

/**********************************************************************/
void scrubHighPrioritySlabs(SlabScrubber  *scrubber,
                            bool           scrubAtLeastOne,
                            VDOCompletion *parent,
                            VDOAction     *callback,
                            VDOAction     *errorHandler)
{
  if (scrubAtLeastOne && isRingEmpty(&scrubber->highPrioritySlabs)) {
    Slab *slab = getNextSlab(scrubber);
    if (slab != NULL) {
      registerSlabForScrubbing(scrubber, slab, true);
    }
  }
  scrubber->highPriorityOnly = true;
  scrubSlabs(scrubber, parent, callback, errorHandler);
}

/**********************************************************************/
void stopScrubbing(SlabScrubber *scrubber, VDOCompletion *parent)
{
  if (isQuiescent(&scrubber->adminState)) {
    completeCompletion(parent);
  } else {
    startDraining(&scrubber->adminState, ADMIN_STATE_SUSPENDING, parent);
  }
}

/**********************************************************************/
void resumeScrubbing(SlabScrubber *scrubber)
{
  if (resumeIfQuiescent(&scrubber->adminState)) {
    scrubNextSlab(scrubber);
  }
}

/**********************************************************************/
int enqueueCleanSlabWaiter(SlabScrubber *scrubber, Waiter *waiter)
{
  if (isReadOnly(scrubber->readOnlyNotifier)) {
    return VDO_READ_ONLY;
  }

  if (isQuiescent(&scrubber->adminState)) {
    return VDO_NO_SPACE;
  }

  return enqueueWaiter(&scrubber->waiters, waiter);
}

/**********************************************************************/
void dumpSlabScrubber(const SlabScrubber *scrubber)
{
  logInfo("slabScrubber slabCount %u waiters %zu %s%s",
          getScrubberSlabCount(scrubber),
          countWaiters(&scrubber->waiters),
          getAdminStateName(&scrubber->adminState),
          scrubber->highPriorityOnly ? ", highPriorityOnly " : "");
}
