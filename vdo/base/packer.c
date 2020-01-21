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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/packer.c#24 $
 */

#include "packerInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminState.h"
#include "allocatingVIO.h"
#include "allocationSelector.h"
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
static inline void assertOnPackerThread(struct packer *packer,
                                        const char    *caller)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == packer->threadID),
                  "%s() called from packer thread", caller);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static inline struct input_bin *inputBinFromRingNode(RingNode *node)
{
  STATIC_ASSERT(offsetof(struct input_bin, ring) == 0);
  return (struct input_bin *) node;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static inline struct output_bin *outputBinFromRingNode(RingNode *node)
{
  STATIC_ASSERT(offsetof(struct output_bin, ring) == 0);
  return (struct output_bin *) node;
}

/**********************************************************************/
struct input_bin *nextBin(const struct packer *packer, struct input_bin *bin)
{
  if (bin->ring.next == &packer->inputBins) {
    return NULL;
  } else {
    return inputBinFromRingNode(bin->ring.next);
  }
}

/**********************************************************************/
struct input_bin *getFullestBin(const struct packer *packer)
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
static void insertInSortedList(struct packer *packer, struct input_bin *bin)
{
  for (struct input_bin *activeBin = getFullestBin(packer);
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
static int makeInputBin(struct packer *packer)
{
  struct input_bin *bin;
  int result = ALLOCATE_EXTENDED(struct input_bin, MAX_COMPRESSION_SLOTS,
                                 struct vio *, __func__, &bin);
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
static void pushOutputBin(struct packer *packer, struct output_bin *bin)
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
static struct output_bin *popOutputBin(struct packer *packer)
{
  if (packer->idleOutputBinCount == 0) {
    return NULL;
  }

  size_t             index = --packer->idleOutputBinCount;
  struct output_bin *bin   = packer->idleOutputBins[index];
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
static int makeOutputBin(struct packer *packer, PhysicalLayer *layer)
{
  struct output_bin *output;
  int result = ALLOCATE(1, struct output_bin, __func__, &output);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Add the bin to the stack even before it's fully initialized so it will
  // be freed even if we fail to initialize it below.
  initializeRing(&output->ring);
  pushRingNode(&packer->outputBins, &output->ring);
  pushOutputBin(packer, output);

  result = ALLOCATE_EXTENDED(struct compressed_block, packer->binDataSize, char,
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
static void freeOutputBin(struct output_bin **binPtr)
{
  struct output_bin *bin = *binPtr;
  if (bin == NULL) {
    return;
  }

  unspliceRingNode(&bin->ring);

  struct vio *vio = allocating_vio_as_vio(bin->writer);
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
               struct packer      **packerPtr)
{
  struct packer *packer;
  int result = ALLOCATE_EXTENDED(struct packer, outputBinCount,
                                 struct output_bin *, __func__, &packer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  packer->threadID       = getPackerZoneThread(threadConfig);
  packer->binDataSize    = VDO_BLOCK_SIZE - sizeof(compressed_block_header);
  packer->size           = inputBinCount;
  packer->maxSlots       = MAX_COMPRESSION_SLOTS;
  packer->outputBinCount = outputBinCount;
  initializeRing(&packer->inputBins);
  initializeRing(&packer->outputBins);

  result = make_allocation_selector(threadConfig->physicalZoneCount,
                                    packer->threadID, &packer->selector);
  if (result != VDO_SUCCESS) {
    freePacker(&packer);
    return result;
  }

  for (BlockCount i = 0; i < inputBinCount; i++) {
    int result = makeInputBin(packer);
    if (result != VDO_SUCCESS) {
      freePacker(&packer);
      return result;
    }
  }

  /*
   * The canceled bin can hold up to half the number of user vios. Every
   * canceled vio in the bin must have a canceler for which it is waiting, and
   * any canceler will only have canceled one lock holder at a time.
   */
  result = ALLOCATE_EXTENDED(struct input_bin, MAXIMUM_USER_VIOS / 2,
                             struct vio *, __func__, &packer->canceledBin);
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
void freePacker(struct packer **packerPtr)
{
  struct packer *packer = *packerPtr;
  if (packer == NULL) {
    return;
  }

  struct input_bin *input;
  while ((input = getFullestBin(packer)) != NULL) {
    unspliceRingNode(&input->ring);
    FREE(input);
  }

  FREE(packer->canceledBin);

  struct output_bin *output;
  while ((output = popOutputBin(packer)) != NULL) {
    freeOutputBin(&output);
  }

  free_allocation_selector(&packer->selector);
  FREE(packer);
  *packerPtr = NULL;
}

/**
 * Get the packer from a data_vio.
 *
 * @param dataVIO  The data_vio
 *
 * @return The packer from the VDO to which the data_vio belongs
 **/
static inline struct packer *getPackerFromDataVIO(struct data_vio *dataVIO)
{
  return getVDOFromDataVIO(dataVIO)->packer;
}

/**********************************************************************/
bool isSufficientlyCompressible(struct data_vio *dataVIO)
{
  struct packer *packer = getPackerFromDataVIO(dataVIO);
  return (dataVIO->compression.size < packer->binDataSize);
}

/**********************************************************************/
ThreadID getPackerThreadID(struct packer *packer)
{
  return packer->threadID;
}

/**********************************************************************/
PackerStatistics getPackerStatistics(const struct packer *packer)
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
 * Abort packing a data_vio.
 *
 * @param dataVIO     The data_vio to abort
 **/
static void abortPacking(struct data_vio *dataVIO)
{
  set_compression_done(dataVIO);
  relaxedAdd64(&getPackerFromDataVIO(dataVIO)->fragmentsPending, -1);
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
  continueDataVIO(dataVIO, VDO_SUCCESS);
}

/**
 * This continues the vio completion without packing the vio.
 *
 * @param waiter  The wait queue entry of the vio to continue
 * @param unused  An argument required so this function may be called
 *                from notifyAllWaiters
 **/
static void
continueVIOWithoutPacking(struct waiter *waiter,
                          void          *unused __attribute__((unused)))
{
  abortPacking(waiterAsDataVIO(waiter));
}

/**
 * Check whether the packer has drained.
 *
 * @param packer  The packer
 **/
static void checkForDrainComplete(struct packer *packer)
{
  if (is_draining(&packer->state)
      && (packer->canceledBin->slotsUsed == 0)
      && (packer->idleOutputBinCount == packer->outputBinCount)) {
    finish_draining(&packer->state);
  }
}

/**********************************************************************/
static void writePendingBatches(struct packer *packer);

/**
 * Ensure that a completion is running on the packer thread.
 *
 * @param completion  The compressed write vio
 *
 * @return <code>true</code> if the completion is on the packer thread
 **/
__attribute__((warn_unused_result))
static bool switchToPackerThread(struct vdo_completion *completion)
{
  struct vio *vio      = asVIO(completion);
  ThreadID    threadID = vio->vdo->packer->threadID;
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
static void finishOutputBin(struct packer *packer, struct output_bin *bin)
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
 * is the vio callback function registered by writeOutputBin().
 *
 * @param completion  The compressed write vio
 **/
static void completeOutputBin(struct vdo_completion *completion)
{
  if (!switchToPackerThread(completion)) {
    return;
  }

  struct vio *vio = asVIO(completion);
  if (completion->result != VDO_SUCCESS) {
    updateVIOErrorStats(vio,
                        "Completing compressed write vio for physical block %"
                        PRIu64 " with error",
                        vio->physical);
  }

  struct packer *packer = vio->vdo->packer;
  finishOutputBin(packer, completion->parent);
  writePendingBatches(packer);
  checkForDrainComplete(packer);
}

/**
 * Implements WaiterCallback. Continues the data_vio waiter.
 **/
static void continueWaiter(struct waiter *waiter,
                           void          *context __attribute__((unused)))
{
  struct data_vio *dataVIO = waiterAsDataVIO(waiter);
  continueDataVIO(dataVIO, VDO_SUCCESS);
}

/**
 * Implements WaiterCallback. Updates the data_vio waiter to refer to its slot
 * in the compressed block, gives the data_vio a share of the PBN lock on that
 * block, and reserves a reference count increment on the lock.
 **/
static void shareCompressedBlock(struct waiter *waiter, void *context)
{
  struct data_vio   *dataVIO = waiterAsDataVIO(waiter);
  struct output_bin *bin     = context;

  dataVIO->newMapped = (struct zoned_pbn) {
    .pbn   = bin->writer->allocation,
    .zone  = bin->writer->zone,
    .state = getStateForSlot(dataVIO->compression.slot),
  };
  dataVIOAsVIO(dataVIO)->physical = dataVIO->newMapped.pbn;

  shareCompressedWriteLock(dataVIO, bin->writer->allocation_lock);

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
static void finishCompressedWrite(struct vdo_completion *completion)
{
  struct output_bin *bin = completion->parent;
  assertInPhysicalZone(bin->writer);

  if (completion->result != VDO_SUCCESS) {
    release_allocation_lock(bin->writer);
    // Invokes completeOutputBin() on the packer thread, which will deal with
    // the waiters.
    vioDoneCallback(completion);
    return;
  }

  // First give every data_vio/HashLock a share of the PBN lock to ensure it
  // can't be released until they've all done their incRefs.
  notifyAllWaiters(&bin->outgoing, shareCompressedBlock, bin);

  // The waiters now hold the (downgraded) PBN lock.
  bin->writer->allocation_lock = NULL;

  // Invokes the callbacks registered before entering the packer.
  notifyAllWaiters(&bin->outgoing, continueWaiter, NULL);

  // Invokes completeOutputBin() on the packer thread.
  vioDoneCallback(completion);
}

/**
 * Continue the write path for a compressed write allocating_vio now that block
 * allocation is complete (the allocating_vio may or may not have actually
 * received an allocation).
 *
 * @param allocatingVIO  The allocating_vio which has finished the allocation
 *                       process
 **/
static void continueAfterAllocation(struct allocating_vio *allocatingVIO)
{
  struct vio            *vio        = allocating_vio_as_vio(allocatingVIO);
  struct vdo_completion *completion = vioAsCompletion(vio);
  if (allocatingVIO->allocation == ZERO_BLOCK) {
    completion->requeue = true;
    setCompletionResult(completion, VDO_NO_SPACE);
    vioDoneCallback(completion);
    return;
  }

  set_physical_zone_callback(allocatingVIO, finishCompressedWrite,
                             THIS_LOCATION("$F(meta);cb=finishCompressedWrite"));
  writeCompressedBlock(allocatingVIO);
}

/**
 * Launch an output bin.
 *
 * @param packer  The packer which owns the bin
 * @param bin     The output bin to launch
 **/
static void launchCompressedWrite(struct packer *packer, struct output_bin *bin)
{
  if (isReadOnly(get_vdo_from_allocating_vio(bin->writer)->readOnlyNotifier)) {
    finishOutputBin(packer, bin);
    return;
  }

  struct vio *vio = allocating_vio_as_vio(bin->writer);
  resetCompletion(vioAsCompletion(vio));
  vio->callback = completeOutputBin;
  vio->priority = VIO_PRIORITY_COMPRESSED_DATA;
  allocate_data_block(bin->writer, packer->selector, VIO_COMPRESSED_WRITE_LOCK,
                      continueAfterAllocation);
}

/**
 * Consume from the pending queue the next batch of vios that can be packed
 * together in a single compressed block. vios that have been mooted since
 * being placed in the pending queue will not be returned.
 *
 * @param packer  The packer
 * @param batch   The counted array to fill with the next batch of vios
 **/
static void getNextBatch(struct packer *packer, struct output_batch *batch)
{
  BlockSize spaceRemaining = packer->binDataSize;
  batch->slotsUsed         = 0;

  struct data_vio *dataVIO;
  while ((dataVIO = waiterAsDataVIO(getFirstWaiter(&packer->batchedDataVIOs)))
         != NULL) {
    // If there's not enough space for the next data_vio, the batch is done.
    if ((dataVIO->compression.size > spaceRemaining)
        || (batch->slotsUsed == packer->maxSlots)) {
      break;
    }

    // Remove the next data_vio from the queue and put it in the output batch.
    dequeueNextWaiter(&packer->batchedDataVIOs);
    batch->slots[batch->slotsUsed++]  = dataVIO;
    spaceRemaining                   -= dataVIO->compression.size;
  }
}

/**
 * Pack the next batch of compressed vios from the batched queue into an
 * output bin and write the output bin.
 *
 * @param packer  The packer
 * @param output  The output bin to fill
 *
 * @return <code>true</code> if a write was issued for the output bin
 **/
__attribute__((warn_unused_result))
static bool writeNextBatch(struct packer *packer, struct output_bin *output)
{
  struct output_batch batch;
  getNextBatch(packer, &batch);

  if (batch.slotsUsed == 0) {
    // The pending queue must now be empty (there may have been mooted vios).
    return false;
  }

  // If the batch contains only a single vio, then we save nothing by saving
  // the compressed form. Continue processing the single vio in the batch.
  if (batch.slotsUsed == 1) {
    abortPacking(batch.slots[0]);
    return false;
  }

  reset_compressed_block_header(&output->block->header);

  size_t spaceUsed = 0;
  for (SlotNumber slot = 0; slot < batch.slotsUsed; slot++) {
    struct data_vio *dataVIO = batch.slots[slot];
    dataVIO->compression.slot = slot;
    put_compressed_block_fragment(output->block, slot, spaceUsed,
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
 * Put a data_vio in a specific input_bin in which it will definitely fit.
 *
 * @param bin      The bin in which to put the data_vio
 * @param dataVIO  The data_vio to add
 **/
static void addToInputBin(struct input_bin *bin, struct data_vio *dataVIO)
{
  dataVIO->compression.bin  = bin;
  dataVIO->compression.slot = bin->slotsUsed;
  bin->incoming[bin->slotsUsed++] = dataVIO;
}

/**
 * Start a new batch of vios in an input_bin, moving the existing batch, if
 * any, to the queue of pending batched vios in the packer.
 *
 * @param packer  The packer
 * @param bin     The bin to prepare
 **/
static void startNewBatch(struct packer *packer, struct input_bin *bin)
{
  // Move all the DataVIOs in the current batch to the batched queue so they
  // will get packed into the next free output bin.
  for (SlotNumber slot = 0; slot < bin->slotsUsed; slot++) {
    struct data_vio *dataVIO = bin->incoming[slot];
    dataVIO->compression.bin = NULL;

    if (!may_write_compressed_data_vio(dataVIO)) {
      /*
       * Compression of this data_vio was canceled while it was waiting; put it
       * in the canceled bin so it can be rendezvous with the canceling
       * data_vio.
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
 * Add a data_vio to a bin's incoming queue, handle logical space change, and
 * call physical space processor.
 *
 * @param packer   The packer
 * @param bin      The bin to which to add the the data_vio
 * @param dataVIO  The data_vio to add to the bin's queue
 **/
static void addDataVIOToInputBin(struct packer    *packer,
                                 struct input_bin *bin,
                                 struct data_vio  *dataVIO)
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
static void writePendingBatches(struct packer *packer)
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

  struct output_bin *output;
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
 * data_vio with other DataVIOs.
 *
 * @param packer   The packer
 * @param dataVIO  The data_vio
 **/
__attribute__((warn_unused_result))
static struct input_bin *selectInputBin(struct packer   *packer,
                                        struct data_vio *dataVIO)
{
  // First best fit: select the bin with the least free space that has enough
  // room for the compressed data in the data_vio.
  struct input_bin *fullestBin = getFullestBin(packer);
  for (struct input_bin *bin = fullestBin;
       bin != NULL;
       bin = nextBin(packer, bin)) {
    if (bin->freeSpace >= dataVIO->compression.size) {
      return bin;
    }
  }

  /*
   * None of the bins have enough space for the data_vio. We're not allowed to
   * create new bins, so we have to overflow one of the existing bins. It's
   * pretty intuitive to select the fullest bin, since that "wastes" the least
   * amount of free space in the compressed block. But if the space currently
   * used in the fullest bin is smaller than the compressed size of the
   * incoming block, it seems wrong to force that bin to write when giving up
   * on compressing the incoming data_vio would likewise "waste" the the least
   * amount of free space.
   */
  if (dataVIO->compression.size
      >= (packer->binDataSize - fullestBin->freeSpace)) {
    return NULL;
  }

  // The fullest bin doesn't have room, but writing it out and starting a new
  // batch with the incoming data_vio will increase the packer's free space.
  return fullestBin;
}

/**********************************************************************/
void attemptPacking(struct data_vio *dataVIO)
{
  struct packer *packer = getPackerFromDataVIO(dataVIO);
  assertOnPackerThread(packer, __func__);

  struct vio_compression_state state = get_compression_state(dataVIO);
  int result = ASSERT((state.status == VIO_COMPRESSING),
                      "attempt to pack data_vio not ready for packing, state: "
                      "%u",
                      state.status);
  if (result != VDO_SUCCESS) {
    return;
  }

  /*
   * Increment whether or not this data_vio will be packed or not since
   * abortPacking() always decrements the counter.
   */
  relaxedAdd64(&packer->fragmentsPending, 1);

  // If packing of this data_vio is disallowed for administrative reasons, give
  // up before making any state changes.
  if (!is_normal(&packer->state)
      || (dataVIO->flushGeneration < packer->flushGeneration)) {
    abortPacking(dataVIO);
    return;
  }

  /*
   * The check of may_block_in_packer() here will set the data_vio's compression
   * state to VIO_PACKING if the data_vio is allowed to be compressed (if it has
   * already been canceled, we'll fall out here). Once the data_vio is in the
   * VIO_PACKING state, it must be guaranteed to be put in an input bin before
   * any more requests can be processed by the packer thread. Otherwise, a
   * canceling data_vio could attempt to remove the canceled data_vio from the
   * packer and fail to rendezvous with it (VDO-2809). We must also make sure
   * that we will actually bin the data_vio and not give up on it as being
   * larger than the space used in the fullest bin. Hence we must call
   * selectInputBin() before calling may_block_in_packer() (VDO-2826).
   */
  struct input_bin *bin = selectInputBin(packer, dataVIO);
  if ((bin == NULL) || !may_block_in_packer(dataVIO)) {
    abortPacking(dataVIO);
    return;
  }

  addDataVIOToInputBin(packer, bin, dataVIO);
  writePendingBatches(packer);
}

/**
 * Force a pending write for all non-empty bins on behalf of a flush or
 * suspend.
 *
 * @param packer  The packer being flushed
 **/
static void writeAllNonEmptyBins(struct packer *packer)
{
  for (struct input_bin *bin = getFullestBin(packer);
       bin != NULL;
       bin = nextBin(packer, bin)) {
    startNewBatch(packer, bin);
    // We don't need to re-sort the bin here since this loop will make every
    // bin have the same amount of free space, so every ordering is sorted.
  }

  writePendingBatches(packer);
}

/**********************************************************************/
void flushPacker(struct packer *packer)
{
  assertOnPackerThread(packer, __func__);
  if (is_normal(&packer->state)) {
    writeAllNonEmptyBins(packer);
  }
}

/*
 * This method is only exposed for unit tests and should not normally be called
 * directly; use removeLockHolderFromPacker() instead.
 */
void removeFromPacker(struct data_vio *dataVIO)
{
  struct input_bin *bin    = dataVIO->compression.bin;
  ASSERT_LOG_ONLY((bin != NULL), "data_vio in packer has an input bin");

  SlotNumber slot = dataVIO->compression.slot;
  bin->slotsUsed--;
  if (slot < bin->slotsUsed) {
    bin->incoming[slot] = bin->incoming[bin->slotsUsed];
    bin->incoming[slot]->compression.slot = slot;
  }

  dataVIO->compression.bin  = NULL;
  dataVIO->compression.slot = 0;

  struct packer *packer = getPackerFromDataVIO(dataVIO);
  if (bin != packer->canceledBin) {
    bin->freeSpace += dataVIO->compression.size;
    insertInSortedList(packer, bin);
  }

  abortPacking(dataVIO);
  checkForDrainComplete(packer);
}

/**********************************************************************/
void removeLockHolderFromPacker(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  assertInPackerZone(dataVIO);

  struct data_vio *lockHolder     = dataVIO->compression.lockHolder;
  dataVIO->compression.lockHolder = NULL;
  removeFromPacker(lockHolder);
}

/**********************************************************************/
void incrementPackerFlushGeneration(struct packer *packer)
{
  assertOnPackerThread(packer, __func__);
  packer->flushGeneration++;
  flushPacker(packer);
}

/**
 * Initiate a drain.
 *
 * Implements AdminInitiator.
 **/
static void initiateDrain(struct admin_state *state)
{
  struct packer *packer = container_of(state, struct packer, state);
  writeAllNonEmptyBins(packer);
  checkForDrainComplete(packer);
}

/**********************************************************************/
void drainPacker(struct packer *packer, struct vdo_completion *completion)
{
  assertOnPackerThread(packer, __func__);
  start_draining(&packer->state, ADMIN_STATE_SUSPENDING, completion,
                 initiateDrain);
}

/**********************************************************************/
void resumePacker(struct packer *packer, struct vdo_completion *parent)
{
  assertOnPackerThread(packer, __func__);
  finishCompletion(parent, resume_if_quiescent(&packer->state));
}

/**********************************************************************/
void resetSlotCount(struct packer *packer, CompressedFragmentCount slots)
{
  if (slots > MAX_COMPRESSION_SLOTS) {
    return;
  }

  packer->maxSlots = slots;
}

/**********************************************************************/
static void dumpInputBin(const struct input_bin *bin, bool canceled)
{
  if (bin->slotsUsed == 0) {
    // Don't dump empty input bins.
    return;
  }

  logInfo("    %sBin slotsUsed=%u freeSpace=%zu",
          (canceled ? "Canceled" : "Input"), bin->slotsUsed, bin->freeSpace);

  // XXX dump vios in bin->incoming? The vios should have been dumped from the
  // vio pool. Maybe just dump their addresses so it's clear they're here?
}

/**********************************************************************/
static void dumpOutputBin(const struct output_bin *bin)
{
  size_t count = countWaiters(&bin->outgoing);
  if (bin->slotsUsed == 0) {
    // Don't dump empty output bins.
    return;
  }

  logInfo("    struct output_bin contains %zu outgoing waiters", count);

  // XXX dump vios in bin->outgoing? The vios should have been dumped from the
  // vio pool. Maybe just dump their addresses so it's clear they're here?

  // XXX dump writer vio?
}

/**********************************************************************/
void dumpPacker(const struct packer *packer)
{
  logInfo("packer");
  logInfo("  flushGeneration=%llu state %s writingBatches=%s",
          packer->flushGeneration, get_admin_state_name(&packer->state),
          boolToString(packer->writingBatches));

  logInfo("  inputBinCount=%llu", packer->size);
  for (struct input_bin *bin = getFullestBin(packer);
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
