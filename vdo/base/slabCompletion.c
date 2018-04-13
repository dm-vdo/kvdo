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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabCompletion.c#1 $
 */

#include "slabCompletion.h"

#include "memoryAlloc.h"

#include "blockAllocator.h"
#include "extent.h"
#include "refCountsInternals.h"
#include "slab.h"
#include "slabJournalInternals.h"
#include "types.h"

/**
 * The SlabCompletion provides functionality to load or save some or all of the
 * slabs from a block allocator. It implements the following workflow which is
 * common to both the load and save operations:
 *
 * 1) Launch an asynchronous operation on the slab journal of every slab.
 * 2) As the slab journal operations complete (in whatever order), check each
 *    slab to see whether its reference counts require I/O. If so, launch that
 *    I/O via the RefCounts' completion.
 * 3) Launch any follow up operations either after the RefCounts I/O has
 *    finished, or immediately if the Slab does not require RefCounts I/O.
 * 4) Once all of the slabs have been processed, finish the completion.
 **/
typedef struct slabCompletion SlabCompletion;

/**
 * A function to start a slab load or save process by launching an operation on
 * the slab's journal.
 *
 * @param slabJournal   The journal to prepare
 * @param parent        The parent completion of the journal operations
 * @param callback      The callback for when the journal is prepared
 * @param errorHandler  The handler for I/O errors
 * @param threadID      The thread on which the callback should run
 **/
typedef void SlabJournalPreparer(SlabJournal   *slabJournal,
                                 VDOCompletion *parent,
                                 VDOAction     *callback,
                                 VDOAction     *errorHandler,
                                 ThreadID       threadID);

/**
 * A function to check whether a slab's reference counts require I/O as part of
 * the current operation.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab requires I/O
 **/
typedef bool SlabStatusChecker(const Slab *slab);

/**
 * A function to perform I/O on a slab's reference counts.
 *
 * @param refCounts     The RefCounts to be read or written
 * @param parent        The completion to notify when the I/O is complete
 * @param callback      The function to call when the I/O is complete
 * @param errorHandler  The handler for I/O errors
 * @param threadID      The thread on which the callback should run
 **/
typedef void ReferenceCountIO(RefCounts     *refCounts,
                              VDOCompletion *parent,
                              VDOAction     *callback,
                              VDOAction     *errorHandler,
                              ThreadID       threadID);

/**
 * A function to be called when processing of a slab is complete. If the slab
 * required I/O, it will be called when the I/O has happened. If it did not
 * require I/O, it will be called when the decision to not do I/O was made.
 *
 * @param slab            The slab whose reference count I/O is finished
 * @param slabCompletion  The completion for the current operation
 **/
typedef void ReferenceCountIOFinisher(Slab           *slab,
                                      SlabCompletion *slabCompletion);

struct slabCompletion {
  VDOCompletion             completion;
  SlabStatusChecker        *needsRefCountIO;
  ReferenceCountIO         *doRefCountIO;
  ReferenceCountIOFinisher *refCountsIOFinished;

  /** The iterator over the slabs */
  SlabIterator              iterator;

  /** The number of slabs remaining to be processed */
  SlabCount                 slabsRemaining;
  /** The thread for running our internal callbacks */
  ThreadID                  threadID;
};

/**
 * Convert a generic completion to a SlabCompletion.
 *
 * @param completion  The SlabCompletion as a VDOCompletion.
 *
 * @return The SlabCompletion
 **/
__attribute__((warn_unused_result))
static SlabCompletion *asSlabCompletion(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(SlabCompletion, completion) == 0);
  assertCompletionType(completion->type, SLAB_COMPLETION);
  return (SlabCompletion *) completion;
}

/**
 * Finish loading or saving reference counts for a slab now that their
 * I/O is done, then launch the load or save of the next slab to be
 * processed. This callback is registered with the extent used to read
 * or write the RefCounts.
 *
 * @param completion  The reference counts completion
 **/
static void finishRefCountsIO(VDOCompletion *completion)
{
  Slab           *slab           = asRefCounts(completion)->slab;
  SlabCompletion *slabCompletion = asSlabCompletion(completion->parent);
  slabCompletion->refCountsIOFinished(slab, slabCompletion);
}

/**
 * Handle an error during loading or saving the reference counts.
 *
 * @param completion  The RefCounts completion.
 **/
static void handleRefCountsIOError(VDOCompletion *completion)
{
  setCompletionResult(completion->parent, completion->result);
  finishRefCountsIO(completion);
}

/**
 * Launch a load or save of the RefCounts in a Slab.
 *
 * @param slabCompletion  the slab completion
 * @param slab            the slab requiring a reference count load or save
 **/
static void doRefCountIO(SlabCompletion *slabCompletion, Slab *slab)
{
  if ((slabCompletion->completion.result != VDO_SUCCESS)
      || !slabCompletion->needsRefCountIO(slab)) {
    // This slab doesn't need to do reference count I/O.
    slabCompletion->refCountsIOFinished(slab, slabCompletion);
    return;
  }

  slabCompletion->doRefCountIO(slab->referenceCounts,
                               &slabCompletion->completion, finishRefCountsIO,
                               handleRefCountsIOError,
                               slabCompletion->threadID);
}

/**
 * Attempt to load or save a slab whose tail block has just been read or
 * flushed.
 *
 * @param completion  The slab journal completion
 **/
static void finishTailBlockIO(VDOCompletion *completion)
{
  SlabCompletion *slabCompletion = asSlabCompletion(completion->parent);
  Slab           *slab           = asSlabJournal(completion)->slab;
  doRefCountIO(slabCompletion, slab);
}

/**
 * Handle an error preparing a slab journal.
 *
 * @param completion  The slab journal completion
 **/
static void handleSlabJournalError(VDOCompletion *completion)
{
  setCompletionResult(completion->parent, completion->result);
  finishTailBlockIO(completion);
}

/**
 * Launch a load or save for all the slabs.
 *
 * @param slabCompletion      The completion for the operation
 * @param prepareSlabJournal  The slab journal I/O function to invoke on
 *                            each slab
 **/
static void launchSlabIO(SlabCompletion       *slabCompletion,
                         SlabJournalPreparer  *prepareSlabJournal)
{
  VDOCompletion *completion      = &slabCompletion->completion;
  slabCompletion->threadID       = getCallbackThreadID();
  slabCompletion->slabsRemaining = 0;
  while (hasNextSlab(&slabCompletion->iterator)) {
    Slab *slab = nextSlab(&slabCompletion->iterator);
    slabCompletion->slabsRemaining++;
    unspliceRingNode(&slab->ringNode);
    if (prepareSlabJournal == NULL) {
      doRefCountIO(slabCompletion, slab);
    } else {
      (*prepareSlabJournal)(slab->journal, completion, finishTailBlockIO,
                            handleSlabJournalError, slabCompletion->threadID);
    }
  }
}

/**********************************************************************/
int makeSlabCompletion(PhysicalLayer *layer, VDOCompletion **completionPtr)
{
  SlabCompletion *slabCompletion;
  int result = ALLOCATE(1, SlabCompletion, __func__, &slabCompletion);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = initializeEnqueueableCompletion(&slabCompletion->completion,
                                           SLAB_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    VDOCompletion *completion = &slabCompletion->completion;
    freeSlabCompletion(&completion);
    return result;
  }

  *completionPtr = &slabCompletion->completion;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeSlabCompletion(VDOCompletion **completionPtr)
{
  if (*completionPtr == NULL) {
    return;
  }

  SlabCompletion *slabCompletion = asSlabCompletion(*completionPtr);
  destroyEnqueueable(&slabCompletion->completion);
  FREE(slabCompletion);
  *completionPtr = NULL;
}

/**
 * Note that the entire operation is finished for some slab. If it was the last
 * slab to be processed, tell our parent that we're done.
 *
 * Implements SlabIOFinisher.
 *
 * @param slab            Ignored
 * @param slabCompletion  The completion for the operation
 **/
static void slabFinished(Slab           *slab __attribute__((unused)),
                         SlabCompletion *slabCompletion)
{
  --slabCompletion->slabsRemaining;
  if (!hasNextSlab(&slabCompletion->iterator)
      && (slabCompletion->slabsRemaining == 0)) {
    finishCompletion(&slabCompletion->completion, VDO_SUCCESS);
  }
}

/**
 * Check whether the reference counts for a given slab should be saved.<p>
 *
 * Implements SlabStatusChecker.
 *
 * @param slab  The slab to check
 **/
static bool shouldSaveReferenceCounts(const Slab *slab)
{
  // If the slab has no dirty reference blocks, saving it immediately returns.
  return !isUnrecoveredSlab(slab);
}

/**
 * Note the closing of a slab, the callback for closeSlabJournal().
 *
 * @param completion  The RefCounts completion
 **/
static void noteSlabIsClosed(VDOCompletion *completion)
{
  setCompletionResult(completion->parent, completion->result);
  slabFinished(NULL, asSlabCompletion(completion->parent));
}

/**
 * Now that the slab journal has been closed, close the reference counts.
 * This is necessary in the case where, due to an error in some other slab,
 * we didn't try to save the reference counts, but we still need to wait in
 * case there was already outstanding reference count I/O.
 *
 * @param completion  The slab journal close completion
 **/
static void waitForReferenceCountsClosed(VDOCompletion *completion)
{
  VDOCompletion *parent = completion->parent;
  setCompletionResult(parent, completion->result);

  Slab *slab = asSlabJournal(completion)->slab;
  if (slab->referenceCounts == NULL) {
    // This can happen when shutting down a VDO that was in read-only mode when
    // loaded.
    noteSlabIsClosed(completion);
    return;
  }

  closeReferenceCounts(slab->referenceCounts, parent, noteSlabIsClosed,
                       noteSlabIsClosed, completion->callbackThreadID);
}

/**
 * Note that the reference counts for a slab have been or won't be written.<p>
 *
 * Implements SlabIOFinisher.
 *
 * @param slab            The slab
 * @param slabCompletion  The completion
 **/
static void referenceCountsWritten(Slab *slab, SlabCompletion *slabCompletion)
{
  closeSlabJournal(slab->journal, &slabCompletion->completion,
                   waitForReferenceCountsClosed, waitForReferenceCountsClosed,
                   slabCompletion->threadID);
}

/**********************************************************************/
void saveSlabs(VDOCompletion *completion, SlabIterator iterator)
{
  SlabCompletion *slabCompletion      = asSlabCompletion(completion);
  slabCompletion->iterator            = iterator;
  slabCompletion->needsRefCountIO     = shouldSaveReferenceCounts;
  slabCompletion->doRefCountIO        = saveReferenceBlocks;
  slabCompletion->refCountsIOFinished = referenceCountsWritten;
  launchSlabIO(slabCompletion, flushSlabJournal);
}

/**********************************************************************/
static bool no(const Slab *slab __attribute__((unused)))
{
  return false;
}

/**********************************************************************/
void loadSlabJournals(VDOCompletion  *completion, SlabIterator iterator)
{
  SlabCompletion *slabCompletion      = asSlabCompletion(completion);
  slabCompletion->iterator            = iterator;
  slabCompletion->needsRefCountIO     = no;
  slabCompletion->doRefCountIO        = NULL;
  slabCompletion->refCountsIOFinished = slabFinished;
  launchSlabIO(slabCompletion, decodeSlabJournal);
}

/**********************************************************************/
void flushSlabJournals(VDOCompletion *completion, SlabIterator iterator)
{
  SlabCompletion *slabCompletion      = asSlabCompletion(completion);
  slabCompletion->iterator            = iterator;
  slabCompletion->needsRefCountIO     = no;
  slabCompletion->doRefCountIO        = NULL;
  slabCompletion->refCountsIOFinished = slabFinished;
  launchSlabIO(slabCompletion, flushSlabJournal);
}

/**
 * Implements SlabStatusChecker.
 **/
static bool yes(const Slab *slab __attribute__((unused)))
{
  return true;
}

/**********************************************************************/
void saveSlab(VDOCompletion *completion, Slab *slab)
{
  SlabCompletion *slabCompletion      = asSlabCompletion(completion);
  slabCompletion->iterator            = iterateSlabs(NULL, 0, 0, 0);
  slabCompletion->slabsRemaining      = 1;
  slabCompletion->needsRefCountIO     = yes;
  slabCompletion->doRefCountIO        = saveReferenceBlocks;
  slabCompletion->refCountsIOFinished = slabFinished;
  doRefCountIO(slabCompletion, slab);
}

/**********************************************************************/
void loadSlabFromLayer(VDOCompletion *completion, Slab *slab)
{
  SlabCompletion *slabCompletion      = asSlabCompletion(completion);
  slabCompletion->iterator            = iterateSlabs(NULL, 0, 0, 0);
  slabCompletion->slabsRemaining      = 1;
  slabCompletion->needsRefCountIO     = yes;
  slabCompletion->doRefCountIO        = loadReferenceBlocks;
  slabCompletion->refCountsIOFinished = slabFinished;
  doRefCountIO(slabCompletion, slab);
}

/**********************************************************************/
void saveFullyRebuiltSlabs(VDOCompletion *completion,
                           SlabIterator   slabIterator)
{
  SlabCompletion *slabCompletion      = asSlabCompletion(completion);
  slabCompletion->iterator            = slabIterator;
  slabCompletion->needsRefCountIO     = shouldSaveFullyBuiltSlab;
  slabCompletion->doRefCountIO        = saveAllReferenceBlocks;
  slabCompletion->refCountsIOFinished = slabFinished;
  launchSlabIO(slabCompletion, NULL);
}
