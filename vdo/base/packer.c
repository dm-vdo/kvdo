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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/packer.c#2 $
 */

#include "packerInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "allocatingVIO.h"
#include "compressionState.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "pbnLock.h"
#include "vdo.h"
#include "vdoInternal.h"

/**
 * Check that we are on the packer thread.
 *
 * @param packer  The packer
 * @param caller  The function which is asserting
 **/
static inline void assertOnPackerThread(Packer *packer, const char *caller)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == packer->threadID),
                  "%s() called from packer thread", caller);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static inline InputBin *inputBinFromRingNode(RingNode *node)
{
  STATIC_ASSERT(offsetof(InputBin, ring) == 0);
  return (InputBin *) node;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static inline OutputBin *outputBinFromRingNode(RingNode *node)
{
  STATIC_ASSERT(offsetof(OutputBin, ring) == 0);
  return (OutputBin *) node;
}

/**********************************************************************/
InputBin *nextBin(const Packer *packer, InputBin *bin)
{
  if (bin->ring.next == &packer->inputBins) {
    return NULL;
  } else {
    return inputBinFromRingNode(bin->ring.next);
  }
}

/**********************************************************************/
InputBin *getFullestBin(const Packer *packer)
{
  if (isRingEmpty(&packer->inputBins)) {
    return NULL;
  } else {
    return inputBinFromRingNode(packer->inputBins.next);
  }
}

/**
 * Insert an input bin to the list, which is in ascending order of free space.
 * Since all bins are already in the list, this actually moves the bin to the
 * correct position in the list.
 *
 * @param packer  The packer
 * @param bin     The input bin to move to its sorted position
 **/
static void insertInSortedList(Packer *packer, InputBin *bin)
{
  for (InputBin *activeBin = getFullestBin(packer);
       activeBin != NULL;
       activeBin = nextBin(packer, activeBin)) {
    if (activeBin->freeSpace > bin->freeSpace) {
      pushRingNode(&activeBin->ring, &bin->ring);
      return;
    }
  }

  pushRingNode(&packer->inputBins, &bin->ring);
}

/**
 * Allocate an input bin and put it into the packer's list.
 *
 * @param packer  The packer
 **/
__attribute__((warn_unused_result))
static int makeInputBin(Packer *packer)
{
  InputBin *bin;
  int result = ALLOCATE_EXTENDED(InputBin, MAX_COMPRESSION_SLOTS, VIO *,
                                 __func__, &bin);
  if (result != VDO_SUCCESS) {
    return result;
  }

  bin->freeSpace = packer->binDataSize;
  initializeRing(&bin->ring);
  pushRingNode(&packer->inputBins, &bin->ring);
  return VDO_SUCCESS;
}

/**
 * Push an output bin onto the stack of idle bins.
 *
 * @param packer  The packer
 * @param bin     The output bin
 **/
static void pushOutputBin(Packer *packer, OutputBin *bin)
{
  ASSERT_LOG_ONLY(!hasWaiters(&bin->outgoing),
                  "idle output bin has no waiters");
  packer->idleOutputBins[packer->idleOutputBinCount++] = bin;
}

/**
 * Pop an output bin off the end of the stack of idle bins.
 *
 * @param packer  The packer
 *
 * @return an idle output bin, or <code>NULL</code> if there are no idle bins
 **/
__attribute__((warn_unused_result))
static OutputBin *popOutputBin(Packer *packer)
{
  if (packer->idleOutputBinCount == 0) {
    return NULL;
  }

  size_t     index = --packer->idleOutputBinCount;
  OutputBin *bin   = packer->idleOutputBins[index];
  packer->idleOutputBins[index] = NULL;
  return bin;
}

/**
 * Allocate a new output bin and push it onto the packer's stack of idle bins.
 *
 * @param packer  The packer
 * @param layer   The physical layer that will receive the compressed block
 *                writes from the output bin
 *
 * @return VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int makeOutputBin(Packer *packer, PhysicalLayer *layer)
{
  OutputBin *output;
  int result = ALLOCATE(1, OutputBin, __func__, &output);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Add the bin to the stack even before it's fully initialized so it will
  // be freed even if we fail to initialize it below.
  initializeRing(&output->ring);
  pushRingNode(&packer->outputBins, &output->ring);
  pushOutputBin(packer, output);

  result = ALLOCATE_EXTENDED(CompressedBlock, packer->binDataSize, char,
                             "compressed block", &output->block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return layer->createCompressedWriteVIO(layer, output, (char *) output->block,
                                         &output->writer);
}

/**
 * Free an idle output bin and null out the reference to it.
 *
 * @param binPtr  The reference to the output bin to free
 **/
static void freeOutputBin(OutputBin **binPtr)
{
  OutputBin *bin = *binPtr;
  if (bin == NULL) {
    return;
  }

  unspliceRingNode(&bin->ring);

  VIO *vio = allocatingVIOAsVIO(bin->writer);
  freeVIO(&vio);
  FREE(bin->block);
  FREE(bin);
  *binPtr = NULL;
}

/**********************************************************************/
int makePacker(PhysicalLayer       *layer,
               BlockCount           inputBinCount,
               BlockCount           outputBinCount,
               const ThreadConfig  *threadConfig,
               Packer             **packerPtr)
{
  Packer *packer;
  int result = ALLOCATE_EXTENDED(Packer, outputBinCount,
                                 OutputBin *, __func__, &packer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  packer->threadID       = getPackerZoneThread(threadConfig);
  packer->binDataSize    = VDO_BLOCK_SIZE - sizeof(CompressedBlockHeader);
  packer->size           = inputBinCount;
  packer->maxSlots       = MAX_COMPRESSION_SLOTS;
  packer->outputBinCount = outputBinCount;
  initializeRing(&packer->inputBins);
  initializeRing(&packer->outputBins);

  for (BlockCount i = 0; i < inputBinCount; i++) {
    int result = makeInputBin(packer);
    if (result != VDO_SUCCESS) {
      freePacker(&packer);
      return result;
    }
  }

  /*
   * The canceled bin can hold up to half the number of user VIOs. Every
   * canceled VIO in the bin must have a canceler for which it is waiting, and
   * any canceler will only have canceled one lock holder at a time.
   */
  result = ALLOCATE_EXTENDED(InputBin, MAXIMUM_USER_VIOS / 2, VIO *, __func__,
                             &packer->canceledBin);
  if (result != VDO_SUCCESS) {
    freePacker(&packer);
    return result;
  }

  for (BlockCount i = 0; i < outputBinCount; i++) {
    int result = makeOutputBin(packer, layer);
    if (result != VDO_SUCCESS) {
      freePacker(&packer);
      return result;
    }
  }

  *packerPtr = packer;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freePacker(Packer **packerPtr)
{
  Packer *packer = *packerPtr;
  if (packer == NULL) {
    return;
  }

  InputBin *input;
  while ((input = getFullestBin(packer)) != NULL) {
    unspliceRingNode(&input->ring);
    FREE(input);
  }

  FREE(packer->canceledBin);

  OutputBin *output;
  while ((output = popOutputBin(packer)) != NULL) {
    freeOutputBin(&output);
  }

  FREE(packer);
  *packerPtr = NULL;
}

/**
 * Get the Packer from a DataVIO.
 *
 * @param dataVIO  The DataVIO
 *
 * @return The Packer from the VDO to which the DataVIO belongs
 **/
static inline Packer *getPackerFromDataVIO(DataVIO *dataVIO)
{
  return getVDOFromDataVIO(dataVIO)->packer;
}

/**********************************************************************/
bool isSufficientlyCompressible(DataVIO *dataVIO)
{
  Packer *packer = getPackerFromDataVIO(dataVIO);
  return (dataVIO->compression.size < packer->binDataSize);
}

/**********************************************************************/
ThreadID getPackerThreadID(Packer *packer)
{
  return packer->threadID;
}

/**********************************************************************/
PackerStatistics getPackerStatistics(const Packer *packer)
{
  /*
   * This is called from getVDOStatistics(), which is called from outside the
   * packer thread. These are just statistics with no semantics that could
   * rely on memory order, so unfenced reads are sufficient.
   */
  return (PackerStatistics) {
    .compressedFragmentsWritten  = relaxedLoad64(&packer->fragmentsWritten),
    .compressedBlocksWritten     = relaxedLoad64(&packer->blocksWritten),
    .compressedFragmentsInPacker = relaxedLoad64(&packer->fragmentsPending),
  };
}

/**
 * Abort packing a DataVIO.
 *
 * @param dataVIO     The DataVIO to abort
 **/
static void abortPacking(DataVIO *dataVIO)
{
  setCompressionDone(dataVIO);
  relaxedAdd64(&getPackerFromDataVIO(dataVIO)->fragmentsPending, -1);
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
  continueDataVIO(dataVIO, VDO_SUCCESS);
}

/**
 * This continues the VIO completion without packing the VIO.
 *
 * @param waiter  The wait queue entry of the VIO to continue
 * @param unused  An argument required so this function may be called
 *                from notifyAllWaiters
 **/
static void continueVIOWithoutPacking(Waiter *waiter,
                                      void *unused __attribute__((unused)))
{
  abortPacking(waiterAsDataVIO(waiter));
}

/**
 * This checks if all VIOs are out of the packer before finishing the
 * completion.
 *
 * @param packer  The packer
 **/
static void checkFlushProgress(Packer *packer)
{
  if (!packer->flushing
      || (packer->canceledBin->slotsUsed > 0)
      || (packer->idleOutputBinCount != packer->outputBinCount)) {
    return;
  }

  packer->flushing = false;
  if (packer->closeRequest != NULL) {
    packer->closed = true;
    finishCompletion(packer->closeRequest, VDO_SUCCESS);
  }
}

/**********************************************************************/
static void writePendingBatches(Packer *packer);

/**
 * Ensure that a completion is running on the packer thread.
 *
 * @param completion  The compressed write VIO
 *
 * @return <code>true</code> if the completion is on the packer thread
 **/
__attribute__((warn_unused_result))
static bool switchToPackerThread(VDOCompletion *completion)
{
  VIO      *vio      = asVIO(completion);
  ThreadID  threadID = vio->vdo->packer->threadID;
  if (completion->callbackThreadID == threadID) {
    return true;
  }

  completion->callbackThreadID = threadID;
  invokeCallback(completion);
  return false;
}

/**
 * Finish processing an output bin whose write has completed. If there was
 * an error, any DataVIOs waiting on the bin write will be notified.
 *
 * @param packer  The packer which owns the bin
 * @param bin     The bin which has finished
 **/
static void finishOutputBin(Packer *packer, OutputBin *bin)
{
  if (hasWaiters(&bin->outgoing)) {
    notifyAllWaiters(&bin->outgoing, continueVIOWithoutPacking, NULL);
  } else {
    // No waiters implies no error, so the compressed block was written.
    relaxedAdd64(&packer->fragmentsPending, -bin->slotsUsed);
    relaxedAdd64(&packer->fragmentsWritten, bin->slotsUsed);
    relaxedAdd64(&packer->blocksWritten, 1);
  }

  bin->slotsUsed = 0;
  pushOutputBin(packer, bin);
}

/**
 * This finishes the bin write process after the bin is written to disk. This
 * is the VIO callback function registered by writeOutputBin().
 *
 * @param completion  The compressed write VIO
 **/
static void completeOutputBin(VDOCompletion *completion)
{
  if (!switchToPackerThread(completion)) {
    return;
  }

  VIO *vio = asVIO(completion);
  if (completion->result != VDO_SUCCESS) {
    logWithStringError(updateVIOErrorStats(vio), completion->result,
                       "Completing compressed write VIO for physical block %"
                       PRIu64 " with error",
                       vio->physical);
  }

  Packer *packer = vio->vdo->packer;
  finishOutputBin(packer, completion->parent);
  writePendingBatches(packer);
  checkFlushProgress(packer);
}

/**
 * Implements WaiterCallback. Continues the DataVIO waiter.
 **/
static void continueWaiter(Waiter *waiter,
                           void   *context __attribute__((unused)))
{
  DataVIO *dataVIO = waiterAsDataVIO(waiter);
  continueDataVIO(dataVIO, VDO_SUCCESS);
}

/**
 * Implements WaiterCallback. Updates the DataVIO waiter to refer to its slot
 * in the compressed block, gives the DataVIO a share of the PBN lock on that
 * block, and reserves a reference count increment on the lock.
 **/
static void shareCompressedBlock(Waiter *waiter, void *context)
{
  DataVIO   *dataVIO = waiterAsDataVIO(waiter);
  OutputBin *bin     = context;

  dataVIO->newMapped = (ZonedPBN) {
    .pbn   = bin->writer->allocation,
    .zone  = bin->writer->zone,
    .state = getStateForSlot(dataVIO->compression.slot),
  };
  dataVIOAsVIO(dataVIO)->physical = dataVIO->newMapped.pbn;

  shareCompressedWriteLock(dataVIO, bin->writer->allocationLock);

  // Wait again for all the waiters to get a share.
  int result = enqueueWaiter(&bin->outgoing, waiter);
  // Cannot fail since this waiter was just dequeued.
  ASSERT_LOG_ONLY(result == VDO_SUCCESS, "impossible enqueueWaiter error");
}

/**
 * Finish a compressed block write. This callback is registered in
 * continueAfterAllocation().
 *
 * @param completion  The compressed write completion
 **/
static void finishCompressedWrite(VDOCompletion *completion)
{
  OutputBin *bin = completion->parent;
  assertInPhysicalZone(bin->writer);

  if (completion->result != VDO_SUCCESS) {
    releaseAllocationLock(bin->writer);
    // Invokes completeOutputBin() on the packer thread, which will deal with
    // the waiters.
    vioDoneCallback(completion);
    return;
  }

  // First give every DataVIO/HashLock a share of the PBN lock to ensure it
  // can't be released until they've all done their incRefs.
  notifyAllWaiters(&bin->outgoing, shareCompressedBlock, bin);

  // The waiters now hold the (downgraded) PBN lock.
  bin->writer->allocationLock = NULL;

  // Invokes the callbacks registered before entering the packer.
  notifyAllWaiters(&bin->outgoing, continueWaiter, NULL);

  // Invokes completeOutputBin() on the packer thread.
  vioDoneCallback(completion);
}

/**
 * Continue the write path for a compressed write AllocatingVIO now that block
 * allocation is complete (the AllocatingVIO may or may not have actually
 * received an allocation).
 *
 * @param allocatingVIO  The AllocatingVIO which has finished the allocation
 *                       process
 **/
static void continueAfterAllocation(AllocatingVIO *allocatingVIO)
{
  VIO           *vio        = allocatingVIOAsVIO(allocatingVIO);
  VDOCompletion *completion = vioAsCompletion(vio);
  if (allocatingVIO->allocation == ZERO_BLOCK) {
    completion->requeue = true;
    setCompletionResult(completion, VDO_NO_SPACE);
    vioDoneCallback(completion);
    return;
  }

  setPhysicalZoneCallback(allocatingVIO, finishCompressedWrite,
                          THIS_LOCATION("$F(meta);cb=finishCompressedWrite"));
  completion->layer->writeCompressedBlock(allocatingVIO);
}

/**
 * Launch an output bin.
 *
 * @param packer  The packer which owns the bin
 * @param bin     The output bin to launch
 **/
static void launchCompressedWrite(Packer *packer, OutputBin *bin)
{
  if (isReadOnly(&getVDOFromAllocatingVIO(bin->writer)->readOnlyContext)) {
    finishOutputBin(packer, bin);
    return;
  }

  VIO *vio = allocatingVIOAsVIO(bin->writer);
  resetCompletion(vioAsCompletion(vio));
  vio->callback = completeOutputBin;
  vio->priority = VIO_PRIORITY_COMPRESSED_DATA;
  allocateDataBlock(bin->writer, VIO_COMPRESSED_WRITE_LOCK,
                    continueAfterAllocation);
}

/**
 * Consume from the pending queue the next batch of VIOs that can be packed
 * together in a single compressed block. VIOs that have been mooted since
 * being placed in the pending queue will not be returned.
 *
 * @param packer  The packer
 * @param batch   The counted array to fill with the next batch of VIOs
 **/
static void getNextBatch(Packer *packer, OutputBatch *batch)
{
  BlockSize spaceRemaining = packer->binDataSize;
  batch->slotsUsed         = 0;

  DataVIO *dataVIO;
  while ((dataVIO = waiterAsDataVIO(getFirstWaiter(&packer->batchedDataVIOs)))
         != NULL) {
    // If there's not enough space for the next DataVIO, the batch is done.
    if ((dataVIO->compression.size > spaceRemaining)
        || (batch->slotsUsed == packer->maxSlots)) {
      break;
    }

    // Remove the next DataVIO from the queue and put it in the output batch.
    dequeueNextWaiter(&packer->batchedDataVIOs);
    batch->slots[batch->slotsUsed++]  = dataVIO;
    spaceRemaining                   -= dataVIO->compression.size;
  }
}

/**
 * Pack the next batch of compressed VIOs from the batched queue into an
 * output bin and write the output bin.
 *
 * @param packer  The packer
 * @param output  The output bin to fill
 *
 * @return <code>true</code> if a write was issued for the output bin
 **/
__attribute__((warn_unused_result))
static bool writeNextBatch(Packer *packer, OutputBin *output)
{
  OutputBatch batch;
  getNextBatch(packer, &batch);

  if (batch.slotsUsed == 0) {
    // The pending queue must now be empty (there may have been mooted VIOs).
    return false;
  }

  // If the batch contains only a single VIO, then we save nothing by saving
  // the compressed form. Continue processing the single VIO in the batch.
  if (batch.slotsUsed == 1) {
    abortPacking(batch.slots[0]);
    return false;
  }

  resetCompressedBlockHeader(&output->block->header);

  size_t spaceUsed = 0;
  for (SlotNumber slot = 0; slot < batch.slotsUsed; slot++) {
    DataVIO *dataVIO = batch.slots[slot];
    dataVIO->compression.slot = slot;
    putCompressedBlockFragment(output->block, slot, spaceUsed,
                               dataVIO->compression.data,
                               dataVIO->compression.size);
    spaceUsed += dataVIO->compression.size;

    int result = enqueueDataVIO(&output->outgoing, dataVIO,
                                THIS_LOCATION(NULL));
    if (result != VDO_SUCCESS) {
      abortPacking(dataVIO);
      continue;
    }

    output->slotsUsed += 1;
  }

  launchCompressedWrite(packer, output);
  return true;
}

/**
 * Put a DataVIO in a specific InputBin in which it will definitely fit.
 *
 * @param bin      The bin in which to put the DataVIO
 * @param dataVIO  The DataVIO to add
 **/
static void addToInputBin(InputBin *bin, DataVIO *dataVIO)
{
  dataVIO->compression.bin  = bin;
  dataVIO->compression.slot = bin->slotsUsed;
  bin->incoming[bin->slotsUsed++] = dataVIO;
}

/**
 * Start a new batch of VIOs in an InputBin, moving the existing batch, if
 * any, to the queue of pending batched VIOs in the packer.
 *
 * @param packer  The packer
 * @param bin     The bin to prepare
 **/
static void startNewBatch(Packer *packer, InputBin *bin)
{
  // Move all the DataVIOs in the current batch to the batched queue so they
  // will get packed into the next free output bin.
  for (SlotNumber slot = 0; slot < bin->slotsUsed; slot++) {
    DataVIO *dataVIO = bin->incoming[slot];
    dataVIO->compression.bin = NULL;

    if (!mayWriteCompressedDataVIO(dataVIO)) {
      /*
       * Compression of this DataVIO was canceled while it was waiting; put it
       * in the canceled bin so it can be rendezvous with the canceling
       * DataVIO.
       */
      addToInputBin(packer->canceledBin, dataVIO);
      continue;
    }

    int result = enqueueDataVIO(&packer->batchedDataVIOs, dataVIO,
                                THIS_LOCATION(NULL));
    if (result != VDO_SUCCESS) {
      // Impossible but we're required to check the result from enqueue.
      abortPacking(dataVIO);
    }
  }

  // The bin is now empty.
  bin->slotsUsed = 0;
  bin->freeSpace = packer->binDataSize;
}

/**
 * Add a DataVIO to a bin's incoming queue, handle logical space change, and
 * call physical space processor.
 *
 * @param packer   The packer
 * @param bin      The bin to which to add the the DataVIO
 * @param dataVIO  The DataVIO to add to the bin's queue
 **/
static void addDataVIOToInputBin(Packer   *packer,
                                 InputBin *bin,
                                 DataVIO  *dataVIO)
{
  // If the selected bin doesn't have room, start a new batch to make room.
  if (bin->freeSpace < dataVIO->compression.size) {
    startNewBatch(packer, bin);
  }

  addToInputBin(bin, dataVIO);
  bin->freeSpace -= dataVIO->compression.size;

  // If we happen to exactly fill the bin, start a new input batch.
  if ((bin->slotsUsed == packer->maxSlots) || (bin->freeSpace == 0)) {
    startNewBatch(packer, bin);
  }

  // Now that we've finished changing the free space, restore the sort order.
  insertInSortedList(packer, bin);
}

/**
 * Move DataVIOs in pending batches from the batchedDataVIOs to all free output
 * bins, issuing writes for the output bins as they are packed. This will loop
 * until either the pending queue is drained or all output bins are busy
 * writing a compressed block.
 *
 * @param packer  The packer
 **/
static void writePendingBatches(Packer *packer)
{
  if (packer->writingBatches) {
    /*
     * We've attempted to re-enter this function recursively due to completion
     * handling, which can lead to kernel stack overflow as in VDO-1340. It's
     * perfectly safe to break the recursion and do nothing since we know any
     * pending batches will eventually be handled by the earlier call.
     */
    return;
  }

  // Record that we are in this function for the above check. IMPORTANT: never
  // return from this function without clearing this flag.
  packer->writingBatches = true;

  OutputBin *output;
  while (hasWaiters(&packer->batchedDataVIOs)
         && ((output = popOutputBin(packer)) != NULL)) {
    if (!writeNextBatch(packer, output)) {
      // We didn't use the output bin to write, so push it back on the stack.
      pushOutputBin(packer, output);
    }
  }

  packer->writingBatches = false;
}

/**
 * Select the input bin that should be used to pack the compressed data in a
 * DataVIO with other DataVIOs.
 *
 * @param packer   The packer
 * @param dataVIO  The DataVIO
 **/
__attribute__((warn_unused_result))
static InputBin *selectInputBin(Packer *packer, DataVIO *dataVIO)
{
  // First best fit: select the bin with the least free space that has enough
  // room for the compressed data in the DataVIO.
  InputBin *fullestBin = getFullestBin(packer);
  for (InputBin *bin = fullestBin; bin != NULL; bin = nextBin(packer, bin)) {
    if (bin->freeSpace >= dataVIO->compression.size) {
      return bin;
    }
  }

  /*
   * None of the bins have enough space for the DataVIO. We're not allowed to
   * create new bins, so we have to overflow one of the existing bins. It's
   * pretty intuitive to select the fullest bin, since that "wastes" the least
   * amount of free space in the compressed block. But if the space currently
   * used in the fullest bin is smaller than the compressed size of the
   * incoming block, it seems wrong to force that bin to write when giving up
   * on compressing the incoming DataVIO would likewise "waste" the the least
   * amount of free space.
   */
  if (dataVIO->compression.size
      >= (packer->binDataSize - fullestBin->freeSpace)) {
    return NULL;
  }

  // The fullest bin doesn't have room, but writing it out and starting a new
  // batch with the incoming DataVIO will increase the packer's free space.
  return fullestBin;
}

/**********************************************************************/
void attemptPacking(DataVIO *dataVIO)
{
  Packer *packer = getPackerFromDataVIO(dataVIO);
  assertOnPackerThread(packer, __func__);

  VIOCompressionState state = getCompressionState(dataVIO);
  int result = ASSERT((state.status == VIO_COMPRESSING),
                      "attempt to pack DataVIO not ready for packing, state: "
                      "%u",
                      state.status);
  if (result != VDO_SUCCESS) {
    return;
  }

  /*
   * Increment whether or not this DataVIO will be packed or not since
   * abortPacking() always decrements the counter.
   */
  relaxedAdd64(&packer->fragmentsPending, 1);

  // If packing of this DataVIO is disallowed for administrative reasons, give
  // up before making any state changes.
  if (packer->closed || packer->flushing
      || (dataVIO->flushGeneration < packer->flushGeneration)) {
    abortPacking(dataVIO);
    return;
  }

  /*
   * The check of mayBlockInPacker() here will set the DataVIO's compression
   * state to VIO_PACKING if the DataVIO is allowed to be compressed (if it has
   * already been canceled, we'll fall out here). Once the DataVIO is in the
   * VIO_PACKING state, it must be guaranteed to be put in an input bin before
   * any more requests can be processed by the packer thread. Otherwise, a
   * canceling DataVIO could attempt to remove the canceled DataVIO from the
   * packer and fail to rendezvous with it (VDO-2809). We must also make sure
   * that we will actually bin the DataVIO and not give up on it as being
   * larger than the space used in the fullest bin. Hence we must call
   * selectInputBin() before calling mayBlockInPacker() (VDO-2826).
   */
  InputBin *bin = selectInputBin(packer, dataVIO);
  if ((bin == NULL) || !mayBlockInPacker(dataVIO)) {
    abortPacking(dataVIO);
    return;
  }

  addDataVIOToInputBin(packer, bin, dataVIO);
  writePendingBatches(packer);
}

/**********************************************************************/
void flushPacker(Packer *packer)
{
  assertOnPackerThread(packer, __func__);
  if (packer->flushing) {
    return;
  }

  packer->flushing = true;

  // Force a pending write for all non-empty bins.
  for (InputBin *bin = getFullestBin(packer);
       bin != NULL;
       bin = nextBin(packer, bin)) {
    startNewBatch(packer, bin);
    // We don't need to re-sort the bin here since this loop will make every
    // bin have the same amount of free space, so every ordering is sorted.
  }

  writePendingBatches(packer);
  checkFlushProgress(packer);
}

/*
 * This method is only exposed for unit tests and should not normally be called
 * directly; use removeLockHolderFromPacker() instead.
 */
void removeFromPacker(DataVIO *dataVIO)
{
  InputBin *bin    = dataVIO->compression.bin;
  ASSERT_LOG_ONLY((bin != NULL), "DataVIO in packer has an input bin");

  SlotNumber slot = dataVIO->compression.slot;
  bin->slotsUsed--;
  if (slot < bin->slotsUsed) {
    bin->incoming[slot] = bin->incoming[bin->slotsUsed];
    bin->incoming[slot]->compression.slot = slot;
  }

  dataVIO->compression.bin  = NULL;
  dataVIO->compression.slot = 0;

  Packer *packer = getPackerFromDataVIO(dataVIO);
  if (bin != packer->canceledBin) {
    bin->freeSpace += dataVIO->compression.size;
    insertInSortedList(packer, bin);
  }

  abortPacking(dataVIO);
  checkFlushProgress(packer);
}

/**********************************************************************/
void removeLockHolderFromPacker(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInPackerZone(dataVIO);

  DataVIO *lockHolder             = dataVIO->compression.lockHolder;
  dataVIO->compression.lockHolder = NULL;
  removeFromPacker(lockHolder);
}

/**********************************************************************/
void incrementPackerFlushGeneration(Packer *packer)
{
  assertOnPackerThread(packer, __func__);
  packer->flushGeneration++;
  flushPacker(packer);
}

/**********************************************************************/
void closePacker(Packer *packer, VDOCompletion *completion)
{
  assertOnPackerThread(packer, __func__);
  ASSERT_LOG_ONLY((packer->closeRequest == NULL),
                  "no simultaneous closePacker() calls");
  packer->closeRequest = completion;
  flushPacker(packer);
}

/**********************************************************************/
void resetSlotCount(Packer *packer, CompressedFragmentCount slots)
{
  if (slots > MAX_COMPRESSION_SLOTS) {
    return;
  }

  packer->maxSlots = slots;
}

/**********************************************************************/
static void dumpInputBin(const InputBin *bin, bool canceled)
{
  if (bin->slotsUsed == 0) {
    // Don't dump empty input bins.
    return;
  }

  logInfo("    %sBin slotsUsed=%u freeSpace=%zu",
          (canceled ? "Canceled" : "Input"), bin->slotsUsed, bin->freeSpace);

  // XXX dump VIOs in bin->incoming? The VIOs should have been dumped from the
  // VIO pool. Maybe just dump their addresses so it's clear they're here?
}

/**********************************************************************/
static void dumpOutputBin(const OutputBin *bin)
{
  size_t count = countWaiters(&bin->outgoing);
  if (bin->slotsUsed == 0) {
    // Don't dump empty output bins.
    return;
  }

  logInfo("    OutputBin contains %zu outgoing waiters", count);

  // XXX dump VIOs in bin->outgoing? The VIOs should have been dumped from the
  // VIO pool. Maybe just dump their addresses so it's clear they're here?

  // XXX dump writer VIO?
}

/**********************************************************************/
void dumpPacker(const Packer *packer)
{
  logInfo("Packer");
  logInfo("  flushGeneration=%" PRIu64
          " flushing=%s closed=%s writingBatches=%s",
          packer->flushGeneration, boolToString(packer->flushing),
          boolToString(packer->closed), boolToString(packer->writingBatches));

  logInfo("  inputBinCount=%" PRIu64, packer->size);
  for (InputBin *bin = getFullestBin(packer);
       bin != NULL;
       bin = nextBin(packer, bin)) {
    dumpInputBin(bin, false);
  }

  dumpInputBin(packer->canceledBin, true);

  logInfo("  outputBinCount=%zu idleOutputBinCount=%zu",
          packer->outputBinCount, packer->idleOutputBinCount);
  const RingNode *head = &packer->outputBins;
  for (RingNode *node = head->next; node != head; node = node->next) {
    dumpOutputBin(outputBinFromRingNode(node));
  }
}
