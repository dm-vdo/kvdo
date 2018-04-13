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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabScrubber.c#1 $
 */

#include "slabScrubberInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockAllocator.h"
#include "readOnlyModeContext.h"
#include "slabRebuild.h"

/**********************************************************************/
int makeSlabScrubber(PhysicalLayer                  *layer,
                     BlockCount                      slabJournalSize,
                     ReadOnlyModeContext            *readOnlyContext,
                     SlabScrubber                  **scrubberPtr)
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
  scrubber->readOnlyContext = readOnlyContext;
  *scrubberPtr              = scrubber;
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

/**********************************************************************/
bool isScrubbing(SlabScrubber *scrubber)
{
  return scrubber->isScrubbing;
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
 * Stop scrubbing, either because we've been told to or because there are
 * no more slabs to scrub.
 *
 * @param scrubber  The scrubber
 **/
static void finishScrubbing(SlabScrubber *scrubber)
{
  scrubber->isScrubbing      = false;
  scrubber->highPriorityOnly = false;
  notifyAllWaiters(&scrubber->waiters, NULL, NULL);
  if (!hasSlabsToScrub(scrubber)) {
    freeSlabRebuildCompletion(&scrubber->slabRebuildCompletion);
  }
  completeCompletion(&scrubber->completion);
}

/**********************************************************************/
static void scrubNextSlab(SlabScrubber *scrubber);

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
  notifyAllWaiters(&scrubber->waiters, NULL, NULL);
  scrubNextSlab(scrubber);
}

/**
 * Scrub the next slab if there is one.
 *
 * @param scrubber  The scrubber
 **/
static void scrubNextSlab(SlabScrubber *scrubber)
{
  if (isReadOnly(scrubber->readOnlyContext)) {
    setCompletionResult(&scrubber->completion, VDO_READ_ONLY);
    finishScrubbing(scrubber);
    return;
  }

  Slab *slab = getNextSlab(scrubber);
  if (scrubber->stopScrubbing || (slab == NULL)
      || (scrubber->highPriorityOnly
          && isRingEmpty(&scrubber->highPrioritySlabs))) {
    finishScrubbing(scrubber);
    return;
  }

  unspliceRingNode(&slab->ringNode);
  resetCompletion(scrubber->slabRebuildCompletion);
  scrubSlab(slab, scrubber->slabRebuildCompletion);
}

/**
 * Handle errors while rebuilding a slab.
 *
 * @param completion  The slab rebuild completion
 **/
static void handleScrubberError(VDOCompletion *completion)
{
  SlabScrubber *scrubber = completion->parent;
  enterReadOnlyMode(scrubber->readOnlyContext, completion->result);
  setCompletionResult(&scrubber->completion, completion->result);
  finishScrubbing(scrubber);
}

/**********************************************************************/
void scrubSlabs(SlabScrubber *scrubber,
                void         *parent,
                VDOAction    *callback,
                VDOAction    *errorHandler,
                ThreadID      threadID)
{
  scrubber->isScrubbing = true;
  prepareCompletion(&scrubber->completion, callback, errorHandler, threadID,
                    parent);
  if (!hasSlabsToScrub(scrubber)) {
    finishScrubbing(scrubber);
    return;
  }

  prepareCompletion(scrubber->slabRebuildCompletion, slabScrubbed,
                    handleScrubberError, getCallbackThreadID(), scrubber);
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
  scrubSlabs(scrubber, parent, callback, errorHandler, getCallbackThreadID());
}

/**********************************************************************/
void stopScrubbing(SlabScrubber *scrubber)
{
  scrubber->stopScrubbing = true;
}

/**********************************************************************/
int enqueueCleanSlabWaiter(SlabScrubber *scrubber, Waiter *waiter)
{
  if (!scrubber->isScrubbing) {
    return (isReadOnly(scrubber->readOnlyContext)
            ? VDO_READ_ONLY : VDO_NO_SPACE);
  }

  return enqueueWaiter(&scrubber->waiters, waiter);
}

/**********************************************************************/
void dumpSlabScrubber(const SlabScrubber *scrubber)
{
  logInfo("slabScrubber slabCount %u waiters %zu %s%s%s",
          getScrubberSlabCount(scrubber),
          countWaiters(&scrubber->waiters),
          scrubber->isScrubbing ? "isScrubbing " : "",
          scrubber->stopScrubbing ? "stopScrubbing " : "",
          scrubber->highPriorityOnly ? "highPriorityOnly " : "");
}
