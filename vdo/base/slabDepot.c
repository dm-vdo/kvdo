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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabDepot.c#5 $
 */

#include "slabDepot.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockAllocatorInternals.h"
#include "constants.h"
#include "header.h"
#include "numUtils.h"
#include "refCounts.h"
#include "slab.h"
#include "slabCompletion.h"
#include "slabDepotInternals.h"
#include "slabJournal.h"
#include "slabJournalEraser.h"
#include "slabIterator.h"
#include "slabSummary.h"
#include "threadConfig.h"
#include "types.h"

typedef struct {
  SlabConfig           slabConfig;
  PhysicalBlockNumber  firstBlock;
  PhysicalBlockNumber  lastBlock;
  ZoneCount            zoneCount;
} __attribute__((packed)) SlabDepotState2_0;

static const Header SLAB_DEPOT_HEADER_2_0 = {
  .id = SLAB_DEPOT,
  .version = {
    .majorVersion = 2,
    .minorVersion = 0,
  },
  .size = sizeof(SlabDepotState2_0),
};

/**
 * Convert a generic VDOCompletion to a SlabDepot.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a SlabDepot
 **/
static inline SlabDepot *asSlabDepot(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(SlabDepot, completion) == 0);
  assertCompletionType(completion->type, SLAB_DEPOT_COMPLETION);
  return (SlabDepot *) completion;
}

/**
 * Compute the number of slabs a depot with given parameters would have.
 *
 * @param firstBlock     PBN of the first data block
 * @param lastBlock      PBN of the last data block
 * @param slabSizeShift  Exponent for the number of blocks per slab
 *
 * @return The number of slabs
 **/
__attribute__((warn_unused_result))
static SlabCount computeSlabCount(PhysicalBlockNumber firstBlock,
                                  PhysicalBlockNumber lastBlock,
                                  unsigned int        slabSizeShift)
{
  BlockCount dataBlocks = lastBlock - firstBlock;
  return (SlabCount) (dataBlocks >> slabSizeShift);
}

/**********************************************************************/
SlabCount calculateSlabCount(SlabDepot *depot)
{
  return computeSlabCount(depot->firstBlock, depot->lastBlock,
                          depot->slabSizeShift);
}

/**
 * Get an iterator over all the slabs in the depot.
 *
 * @param depot  The depot
 *
 * @return An iterator over the depot's slabs
 **/
static SlabIterator getSlabIterator(SlabDepot *depot)
{
  return iterateSlabs(depot->slabs, depot->slabCount - 1, 0, 1);
}

/**
 * Allocate a new slab pointer array. Any existing slab pointers will be
 * copied into the new array, and slabs will be allocated as needed. The
 * newly allocated slabs will not be distributed for use by the block
 * allocators.
 *
 * @param depot      The depot
 * @param layer      The layer to use
 * @param slabCount  The number of slabs the depot should have in the new
 *                   array
 *
 * @return VDO_SUCCESS or an error code
 **/
static int allocateSlabs(SlabDepot     *depot,
                         PhysicalLayer *layer,
                         SlabCount      slabCount)
{
  int result = ALLOCATE(slabCount, Slab *, "slab pointer array",
                        &depot->newSlabs);
  if (result != VDO_SUCCESS) {
    return result;
  }

  bool resizing = false;
  if (depot->slabs != NULL) {
    memcpy(depot->newSlabs, depot->slabs, depot->slabCount * sizeof(Slab *));
    resizing = true;
  }

  BlockCount slabSize = getSlabConfig(depot)->slabBlocks;
  PhysicalBlockNumber slabOrigin
    = depot->firstBlock + (depot->slabCount * slabSize);

  // The translation between allocator partition PBNs and layer PBNs.
  BlockCount translation = depot->origin - depot->firstBlock;
  depot->newSlabCount = depot->slabCount;
  while (depot->newSlabCount < slabCount) {
    BlockAllocator *allocator
      = depot->allocators[depot->newSlabCount % depot->zoneCount];
    Slab **slabPtr = &depot->newSlabs[depot->newSlabCount];
    result = makeSlab(slabOrigin, allocator, translation, depot->journal,
                      layer, depot->newSlabCount, slabPtr);
    if (result != VDO_SUCCESS) {
      return result;
    }
    // Increment here to ensure that abandonNewSlabs will clean up correctly.
    depot->newSlabCount++;

    slabOrigin += slabSize;

    if (resizing) {
      result = allocateRefCountsForSlab(layer, *slabPtr);
      if (result != VDO_SUCCESS) {
        return result;
      }
    }
  }
  return VDO_SUCCESS;
}

/**********************************************************************/
void abandonNewSlabs(SlabDepot *depot)
{
  if (depot->newSlabs == NULL) {
    return;
  }
  for (SlabCount i = depot->slabCount; i < depot->newSlabCount; i++) {
    freeSlab(&depot->newSlabs[i]);
  }
  depot->newSlabCount = 0;
  FREE(depot->newSlabs);
  depot->newSlabs = NULL;
  depot->newSize  = 0;
}

/**
 * Allocate those components of the slab depot which are needed only at load
 * time, not at format time.
 *
 * @param depot             The depot
 * @param nonce             The nonce of the VDO
 * @param threadConfig      The thread config of the VDO
 * @param vioPoolSize       The size of the VIO pool
 * @param layer             The physical layer below this depot
 * @param summaryPartition  The partition which holds the slab summary
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocateComponents(SlabDepot          *depot,
                              Nonce               nonce,
                              const ThreadConfig *threadConfig,
                              BlockCount          vioPoolSize,
                              PhysicalLayer      *layer,
                              Partition          *summaryPartition)
{
  /*
   * If createVIO is NULL, the slab depot is only being used to format
   * or audit the VDO. These only require the SuperBlock component, so we can
   * just skip allocating all the memory needed for runtime components.
   */
  if (layer->createMetadataVIO == NULL) {
    return VDO_SUCCESS;
  }

  int result = initializeEnqueueableCompletion(&depot->completion,
                                               SLAB_DEPOT_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = initializeEnqueueableCompletion(&depot->actionCompletion,
                                           SUB_TASK_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = initializeEnqueueableCompletion(&depot->saveCompletion,
                                           SUB_TASK_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = initializeEnqueueableCompletion(&depot->subTaskCompletion,
                                           SUB_TASK_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  depot->recoveryJournalThreadID = getJournalZoneThread(threadConfig);
  depot->origin = depot->firstBlock;

  result = makeSlabSummary(layer, summaryPartition, threadConfig,
                           depot->slabSizeShift, depot->slabConfig.dataBlocks,
                           depot->readOnlyContext, &depot->slabSummary);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeSlabCompletion(layer, &depot->slabCompletion);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SlabCount slabCount = calculateSlabCount(depot);
  if (threadConfig->physicalZoneCount > slabCount) {
    return logErrorWithStringError(VDO_BAD_CONFIGURATION,
                                   "%u physical zones exceeds slab count %u",
                                   threadConfig->physicalZoneCount, slabCount);
  }

  // Allocate the block allocators.
  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    ThreadID threadID = getPhysicalZoneThread(threadConfig, zone);
    result = makeBlockAllocator(depot, zone, threadID, nonce, vioPoolSize,
                                layer, depot->readOnlyContext,
                                &depot->allocators[zone]);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  // Allocate slabs.
  result = allocateSlabs(depot, layer, slabCount);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Use the new slabs.
  for (SlabCount i = depot->slabCount; i < depot->newSlabCount; i++) {
    Slab *slab = depot->newSlabs[i];
    registerSlabWithAllocator(slab->allocator, slab, false);
    depot->slabCount++;
  }

  depot->slabs        = depot->newSlabs;
  depot->newSlabs     = NULL;
  depot->newSlabCount = 0;

  return VDO_SUCCESS;
}

/**
 * Allocate a slab depot.
 *
 * @param [in]  state             The parameters for the new depot
 * @param [in]  threadConfig      The thread config of the VDO
 * @param [in]  nonce             The nonce of the VDO
 * @param [in]  vioPoolSize       The size of the VIO pool
 * @param [in]  layer             The physical layer below this depot
 * @param [in]  summaryPartition  The partition which holds the slab
 *                                summary (if NULL, the depot is format-only)
 * @param [in]  readOnlyContext   The context for entering read-only mode
 * @param [in]  recoveryJournal   The recovery journal of the VDO
 * @param [out] depotPtr          A pointer to hold the depot
 *
 * @return A success or error code
 **/
__attribute__((warn_unused_result))
static int allocateDepot(const SlabDepotState2_0  *state,
                         const ThreadConfig       *threadConfig,
                         Nonce                     nonce,
                         BlockCount                vioPoolSize,
                         PhysicalLayer            *layer,
                         Partition                *summaryPartition,
                         ReadOnlyModeContext      *readOnlyContext,
                         RecoveryJournal          *recoveryJournal,
                         SlabDepot               **depotPtr)
{
  // Calculate the bit shift for efficiently mapping block numbers to slabs.
  // Using a shift requires that the slab size be a power of two.
  BlockCount slabSize = state->slabConfig.slabBlocks;
  if (!isPowerOfTwo(slabSize)) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "slab size must be a power of two");
  }
  unsigned int slabSizeShift = logBaseTwo(slabSize);

  SlabDepot *depot;
  int result = ALLOCATE_EXTENDED(SlabDepot, threadConfig->physicalZoneCount,
                                 BlockAllocator *, __func__, &depot);
  if (result != VDO_SUCCESS) {
    return result;
  }

  depot->oldZoneCount    = state->zoneCount;
  depot->zoneCount       = threadConfig->physicalZoneCount;
  depot->slabConfig      = state->slabConfig;
  depot->readOnlyContext = readOnlyContext;
  depot->firstBlock      = state->firstBlock;
  depot->lastBlock       = state->lastBlock;
  depot->slabSizeShift   = slabSizeShift;
  depot->journal         = recoveryJournal;

  result = allocateComponents(depot, nonce, threadConfig, vioPoolSize,
                              layer, summaryPartition);
  if (result != VDO_SUCCESS) {
    freeSlabDepot(&depot);
    return result;
  }

  *depotPtr = depot;
  return VDO_SUCCESS;
}

/**
 * Configure the SlabDepot for the specified storage capacity, finding the
 * number of data blocks that will fit and still leave room for the depot
 * metadata, then return the saved state for that configuration.
 *
 * @param [in]  blockCount  The number of blocks in the underlying storage
 * @param [in]  firstBlock  The number of the first block that may be allocated
 * @param [in]  slabConfig  The configuration of a single slab
 * @param [in]  zoneCount   The number of zones the depot will use
 * @param [out] state       The state structure to be configured
 *
 * @return VDO_SUCCESS or an error code
 **/
static int configureState(BlockCount           blockCount,
                          PhysicalBlockNumber  firstBlock,
                          SlabConfig           slabConfig,
                          ZoneCount            zoneCount,
                          SlabDepotState2_0   *state)
{
  BlockCount slabSize = slabConfig.slabBlocks;
  logDebug("slabDepot configureState(blockCount=%" PRIu64
           ", firstBlock=%" PRIu64 ", slabSize=%" PRIu64 ", zoneCount=%u)",
           blockCount, firstBlock, slabSize, zoneCount);

  // We do not allow runt slabs, so we waste up to a slab's worth.
  size_t slabCount = (blockCount / slabSize);
  if (slabCount == 0) {
    return VDO_NO_SPACE;
  }

  if (slabCount > MAX_SLABS) {
    return VDO_TOO_MANY_SLABS;
  }

  BlockCount          totalSlabBlocks = slabCount * slabConfig.slabBlocks;
  BlockCount          totalDataBlocks = slabCount * slabConfig.dataBlocks;
  PhysicalBlockNumber lastBlock       = firstBlock + totalSlabBlocks;

  *state = (SlabDepotState2_0) {
    .slabConfig = slabConfig,
    .firstBlock = firstBlock,
    .lastBlock  = lastBlock,
    .zoneCount  = zoneCount,
  };

  logDebug("slabDepot lastBlock=%" PRIu64 ", totalDataBlocks=%" PRIu64
           ", slabCount=%zu, leftOver=%" PRIu64,
           lastBlock, totalDataBlocks, slabCount,
           blockCount - (lastBlock - firstBlock));

  return VDO_SUCCESS;
}

/**********************************************************************/
int makeSlabDepot(BlockCount            blockCount,
                  PhysicalBlockNumber   firstBlock,
                  SlabConfig            slabConfig,
                  const ThreadConfig   *threadConfig,
                  Nonce                 nonce,
                  BlockCount            vioPoolSize,
                  PhysicalLayer        *layer,
                  Partition            *summaryPartition,
                  ReadOnlyModeContext  *readOnlyContext,
                  RecoveryJournal      *recoveryJournal,
                  SlabDepot           **depotPtr)
{
  SlabDepotState2_0 state;
  int result = configureState(blockCount, firstBlock, slabConfig, 0, &state);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SlabDepot *depot = NULL;
  result = allocateDepot(&state, threadConfig, nonce, vioPoolSize, layer,
                         summaryPartition, readOnlyContext, recoveryJournal,
                         &depot);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *depotPtr = depot;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeSlabDepot(SlabDepot **depotPtr)
{
  SlabDepot *depot = *depotPtr;
  if (depot == NULL) {
    return;
  }

  abandonNewSlabs(depot);

  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    freeBlockAllocator(&depot->allocators[zone]);
  }

  if (depot->slabs != NULL) {
    for (SlabCount i = 0; i < depot->slabCount; i++) {
      freeSlab(&depot->slabs[i]);
    }
  }

  FREE(depot->slabs);
  freeSlabCompletion(&depot->slabCompletion);
  freeSlabSummary(&depot->slabSummary);
  destroyEnqueueable(&depot->subTaskCompletion);
  destroyEnqueueable(&depot->saveCompletion);
  destroyEnqueueable(&depot->actionCompletion);
  destroyEnqueueable(&depot->completion);
  FREE(depot);
  *depotPtr = NULL;
}

/**********************************************************************/
size_t getSlabDepotEncodedSize(void)
{
  return ENCODED_HEADER_SIZE + sizeof(SlabDepotState2_0);
}

/**
 * Decode a slab config from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The config structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodeSlabConfig(Buffer *buffer, SlabConfig *config)
{
  int result = getUInt64LEFromBuffer(buffer, &config->slabBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &config->dataBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &config->referenceCountBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &config->slabJournalBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result
    = getUInt64LEFromBuffer(buffer, &config->slabJournalFlushingThreshold);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result
    = getUInt64LEFromBuffer(buffer, &config->slabJournalBlockingThreshold);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return getUInt64LEFromBuffer(buffer, &config->slabJournalScrubbingThreshold);
}

/**
 * Encode a slab config into a buffer.
 *
 * @param config  The config structure to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int encodeSlabConfig(const SlabConfig *config, Buffer *buffer)
{
  int result = putUInt64LEIntoBuffer(buffer, config->slabBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->dataBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->referenceCountBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->slabJournalBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->slabJournalFlushingThreshold);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->slabJournalBlockingThreshold);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return putUInt64LEIntoBuffer(buffer, config->slabJournalScrubbingThreshold);
}

/**********************************************************************/
int encodeSlabDepot(const SlabDepot *depot, Buffer *buffer)
{
  int result = encodeHeader(&SLAB_DEPOT_HEADER_2_0, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t initialLength = contentLength(buffer);

  result = encodeSlabConfig(&depot->slabConfig, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, depot->firstBlock);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, depot->lastBlock);
  if (result != UDS_SUCCESS) {
    return result;
  }

  /*
   * If this depot is currently using 0 zones, it must have been
   * synchronously loaded by a tool and is now being saved. We
   * did not load and combine the slab summary, so we still need
   * to do that next time we load with the old zone count rather
   * than 0.
   */
  ZoneCount zonesToRecord = depot->zoneCount;
  if (depot->zoneCount == 0) {
    zonesToRecord = depot->oldZoneCount;
  }
  result = putByte(buffer, zonesToRecord);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t encodedSize = contentLength(buffer) - initialLength;
  return ASSERT(SLAB_DEPOT_HEADER_2_0.size == encodedSize,
                "encoded block map component size must match header size");
}

/**
 * Decode slab depot component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodeSlabDepotState_2_0(Buffer *buffer, SlabDepotState2_0 *state)
{
  size_t initialLength = contentLength(buffer);

  int result = decodeSlabConfig(buffer, &state->slabConfig);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->firstBlock);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->lastBlock);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getByte(buffer, &state->zoneCount);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t decodedSize = initialLength - contentLength(buffer);
  return ASSERT(SLAB_DEPOT_HEADER_2_0.size == decodedSize,
                "decoded slab depot component size must match header size");
}

/**********************************************************************/
int decodeSlabDepot(Buffer               *buffer,
                    const ThreadConfig   *threadConfig,
                    Nonce                 nonce,
                    PhysicalLayer        *layer,
                    Partition            *summaryPartition,
                    ReadOnlyModeContext  *readOnlyContext,
                    RecoveryJournal      *recoveryJournal,
                    SlabDepot           **depotPtr)
{
  Header header;
  int result = decodeHeader(buffer, &header);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = validateHeader(&SLAB_DEPOT_HEADER_2_0, &header, true, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SlabDepotState2_0 state;
  result = decodeSlabDepotState_2_0(buffer, &state);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return allocateDepot(&state, threadConfig, nonce, VIO_POOL_SIZE, layer,
                       summaryPartition, readOnlyContext, recoveryJournal,
                       depotPtr);
}

/**********************************************************************/
int decodeSodiumSlabDepot(Buffer               *buffer,
                          const ThreadConfig   *threadConfig,
                          Nonce                 nonce,
                          PhysicalLayer        *layer,
                          Partition            *summaryPartition,
                          ReadOnlyModeContext  *readOnlyContext,
                          RecoveryJournal      *recoveryJournal,
                          SlabDepot           **depotPtr)
{
  // Sodium uses version 2.0 of the slab depot state.
  return decodeSlabDepot(buffer, threadConfig, nonce, layer, summaryPartition,
                         readOnlyContext, recoveryJournal, depotPtr);
}

/**********************************************************************/
int allocateSlabRefCounts(SlabDepot *depot, PhysicalLayer *layer)
{
  SlabIterator iterator = getSlabIterator(depot);
  while (hasNextSlab(&iterator)) {
    int result = allocateRefCountsForSlab(layer, nextSlab(&iterator));
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
BlockAllocator *getBlockAllocatorForZone(SlabDepot *depot,
                                         ZoneCount  zoneNumber)
{
  return depot->allocators[zoneNumber];
}

/**********************************************************************/
int getSlabNumber(const SlabDepot     *depot,
                  PhysicalBlockNumber  pbn,
                  SlabCount           *slabNumberPtr)
{
  if (pbn < depot->firstBlock) {
    return VDO_OUT_OF_RANGE;
  }

  SlabCount slabNumber = (pbn - depot->firstBlock) >> depot->slabSizeShift;
  if (slabNumber >= depot->slabCount) {
    return VDO_OUT_OF_RANGE;
  }

  *slabNumberPtr = slabNumber;
  return VDO_SUCCESS;
}

/**********************************************************************/
Slab *getSlab(const SlabDepot *depot, PhysicalBlockNumber pbn)
{
  if (pbn == ZERO_BLOCK) {
    return NULL;
  }

  SlabCount slabNumber;
  int result = getSlabNumber(depot, pbn, &slabNumber);
  if (result != VDO_SUCCESS) {
    enterReadOnlyMode(depot->readOnlyContext, result);
    return NULL;
  }

  return depot->slabs[slabNumber];

}

/**********************************************************************/
SlabJournal *getSlabJournal(const SlabDepot *depot, PhysicalBlockNumber pbn)
{
  Slab *slab = getSlab(depot, pbn);
  return ((slab != NULL) ? slab->journal : NULL);
}

/**********************************************************************/
uint8_t getIncrementLimit(SlabDepot *depot, PhysicalBlockNumber pbn)
{
  Slab *slab = getSlab(depot, pbn);
  if ((slab == NULL) || isUnrecoveredSlab(slab)) {
    return 0;
  }

  return getAvailableReferences(slab->referenceCounts, pbn);
}

/**********************************************************************/
bool isPhysicalDataBlock(const SlabDepot *depot, PhysicalBlockNumber pbn)
{
  if (pbn == ZERO_BLOCK) {
    return true;
  }

  SlabCount slabNumber;
  if (getSlabNumber(depot, pbn, &slabNumber) != VDO_SUCCESS) {
    return false;
  }

  SlabBlockNumber sbn;
  int result = slabBlockNumberFromPBN(depot->slabs[slabNumber], pbn, &sbn);
  return (result == VDO_SUCCESS);
}

/**********************************************************************/
BlockCount getDepotAllocatedBlocks(const SlabDepot *depot)
{
  BlockCount total = 0;
  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    // The allocators are responsible for thread safety.
    total += getAllocatedBlocks(depot->allocators[zone]);
  }
  return total;
}

/**********************************************************************/
BlockCount getDepotDataBlocks(const SlabDepot *depot)
{
  // XXX This needs to be thread safe, but resize changes the slab count. It
  // does so on the admin thread (our usual caller), so it's usually safe.
  return (depot->slabCount * depot->slabConfig.dataBlocks);
}

/**********************************************************************/
BlockCount getDepotFreeBlocks(const SlabDepot *depot)
{
  /*
   * We can't ever shrink a volume except when resize fails, and we can't
   * allocate from the new slabs until after the resize succeeds, so by
   * getting the number of allocated blocks first, we ensure the allocated
   * count is always less than the capacity. Doing it in the other order on a
   * full volume could lose a race with a sucessful resize, resulting in a
   * nonsensical negative/underflow result.
   */
  BlockCount allocated = getDepotAllocatedBlocks(depot);
  memoryFence();
  return (getDepotDataBlocks(depot) - allocated);
}

/**********************************************************************/
SlabCount getDepotSlabCount(const SlabDepot *depot)
{
  return depot->slabCount;
}

/**********************************************************************/
SlabCount getDepotUnrecoveredSlabCount(const SlabDepot *depot)
{
  SlabCount total = 0;
  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    // The allocators are responsible for thread safety.
    total += getUnrecoveredSlabCount(depot->allocators[zone]);
  }
  return total;
}

/**********************************************************************/
static bool abortLoadOnError(VDOCompletion *completion)
{
  if (completion->result == VDO_SUCCESS) {
    return false;
  }

  SlabDepot *depot = completion->parent;
  finishCompletion(depot->slabCompletion, completion->result);
  return true;
}

/**
 * Finish loading the slab summary and begin loading slabs. This is the
 * callback for the slab summary load registered in loadSlabDepot().
 *
 * @param summaryCompletion  The SlabSummary load completion
 **/
static void finishLoadingSummary(VDOCompletion *summaryCompletion)
{
  if (abortLoadOnError(summaryCompletion)) {
    return;
  }

  SlabDepot *depot = summaryCompletion->parent;
  loadSlabJournals(depot->slabCompletion, getSlabIterator(depot));
}

/**********************************************************************/
void loadSlabDepot(SlabDepot *depot, bool formatDepot, VDOCompletion *parent)
{
  int result = allocateSlabRefCounts(depot, parent->layer);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  if (formatDepot) {
    loadSlabSummary(depot->slabSummary, depot->oldZoneCount, parent,
                    finishParentCallback);
    return;
  }

  prepareToFinishParent(depot->slabCompletion, parent);
  loadSlabSummary(depot->slabSummary, depot->oldZoneCount,
                  depot, finishLoadingSummary);
}

/**
 * Finish loading the slab summary and begin loading slab journals.
 * This is the callback for the slab summary load registered in
 * loadSlabDepotForRecovery().
 *
 * @param summaryCompletion  The SlabSummary load completion
 **/
static void loadSlabJournalsForRecovery(VDOCompletion *summaryCompletion)
{
  if (abortLoadOnError(summaryCompletion)) {
    return;
  }

  SlabDepot *depot = summaryCompletion->parent;
  loadSlabJournals(depot->slabCompletion, getSlabIterator(depot));
}

/**********************************************************************/
void loadSlabDepotForRecovery(SlabDepot *depot, VDOCompletion *parent)
{
  prepareToFinishParent(depot->slabCompletion, parent);
  loadSlabSummary(depot->slabSummary, depot->oldZoneCount, depot,
                  loadSlabJournalsForRecovery);
}

/**
 * Finish erasing the slab summary and begin erasing slab journals.
 * This is the callback for the slab summary 'load' registered in
 * loadSlabDepotForRebuild().
 *
 * @param summaryCompletion  The SlabSummary load completion
 **/
static void eraseSlabJournalsForRebuild(VDOCompletion *summaryCompletion)
{
  if (abortLoadOnError(summaryCompletion)) {
    return;
  }

  SlabDepot *depot = summaryCompletion->parent;
  eraseSlabJournals(depot, getSlabIterator(depot), depot->slabCompletion);
}

/**********************************************************************/
void loadSlabDepotForRebuild(SlabDepot *depot, VDOCompletion *parent)
{
  prepareToFinishParent(depot->slabCompletion, parent);
  loadSlabSummary(depot->slabSummary, 0, depot, eraseSlabJournalsForRebuild);
}

/**
 * Handle an error when applying an action to an allocator.
 *
 * @param completion  The depot completion
 **/
static void handleAllocatorError(VDOCompletion *completion)
{
  // Preserve the error.
  setCompletionResult(completion->parent, completion->result);

  // Carry on.
  resetCompletion(completion);
  invokeCallback(completion);
}

/**
 * Perform an action on the next allocator if there is one.
 *
 * @param completion  The depot completion
 **/
static void applyToNextAllocator(VDOCompletion *completion)
{
  SlabDepot      *depot     = asSlabDepot(completion);
  BlockAllocator *allocator = depot->allocators[depot->actingZone];
  ASSERT_LOG_ONLY((getCallbackThreadID() == allocator->threadID),
                  "applyToNextAllocator() called on next allocator's thread");

  depot->actingZone++;
  if (depot->actingZone == depot->zoneCount) {
    // We are about to apply to the last allocator, so once that is finished,
    // we're done.
    prepareToFinishParent(completion, completion->parent);
  } else {
    // Prepare to come back in the zone of the next allocator.
    prepareCompletion(completion, applyToNextAllocator, handleAllocatorError,
                      depot->allocators[depot->actingZone]->threadID,
                      completion->parent);
  }
  depot->action(allocator, completion, finishParentCallback,
                finishParentCallback);
}

/**
 * Launch an action to be applied to each block allocator in turn in that
 * allocator's zone.
 *
 * @param depot   The slab depot
 * @param action  The action to perform
 * @param parent  The object to notify when the action has been applied to
 *                all the allocators
 **/
static void applyToAllAllocators(SlabDepot       *depot,
                                 AllocatorAction *action,
                                 VDOCompletion   *parent)
{
  depot->action     = action;
  depot->actingZone = 0;
  prepareCompletion(&depot->completion, applyToNextAllocator,
                    handleAllocatorError,
                    depot->allocators[depot->actingZone]->threadID, parent);
  invokeCallback(&depot->completion);
}

/**
 * Prepare the depot's action completion and then apply the action to each
 * allocator in turn via applyToAllAllocators().
 *
 * @param depot         The slab depot
 * @param action        The action to perform
 * @param parent        The object to notify when the action has been applied
 *                      to all the allocators
 * @param callback      The function to call when the action has been applied
 *                      to all the allocators
 * @param errorHandler  The error handler for the action
 **/
static void launchOnAllAllocators(SlabDepot       *depot,
                                  AllocatorAction *action,
                                  void            *parent,
                                  VDOAction       *callback,
                                  VDOAction       *errorHandler)
{
  prepareCompletion(&depot->actionCompletion, callback, errorHandler,
                    getCallbackThreadID(), parent);
  applyToAllAllocators(depot, action, &depot->actionCompletion);
}

/**********************************************************************/
void prepareToAllocate(SlabDepot         *depot,
                       SlabDepotLoadType  loadType,
                       VDOCompletion     *parent)
{
  depot->loadType = loadType;
  atomicStore32(&depot->zonesToScrub, depot->zoneCount);
  launchOnAllAllocators(depot, prepareAllocatorToAllocate, parent,
                        finishParentCallback, finishParentCallback);
}

/**********************************************************************/
void flushDepotSlabJournals(SlabDepot     *depot,
                            VDOCompletion *parent,
                            VDOAction     *callback,
                            VDOAction     *errorHandler)
{
  launchOnAllAllocators(depot, flushAllocatorSlabJournals, parent, callback,
                        errorHandler);
}

/**********************************************************************/
void updateSlabDepotSize(SlabDepot *depot, bool reverting)
{
  depot->lastBlock = (reverting ? depot->oldLastBlock : depot->newLastBlock);
}

/**********************************************************************/
int prepareToGrowSlabDepot(SlabDepot *depot, BlockCount newSize)
{
  if ((newSize >> depot->slabSizeShift) <= depot->slabCount) {
    return VDO_INCREMENT_TOO_SMALL;
  }

  // Generate the depot configuration for the new block count.
  SlabDepotState2_0 newState;
  int result = configureState(newSize, depot->firstBlock, depot->slabConfig,
                              depot->zoneCount, &newState);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SlabCount newSlabCount = computeSlabCount(depot->firstBlock,
                                            newState.lastBlock,
                                            depot->slabSizeShift);
  if (newSlabCount <= depot->slabCount) {
    return logErrorWithStringError(VDO_INCREMENT_TOO_SMALL,
                                   "Depot can only grow");
  }
  if (newSlabCount == depot->newSlabCount) {
    // Check it out, we've already got all the new slabs allocated!
    return VDO_SUCCESS;
  }

  abandonNewSlabs(depot);
  result = allocateSlabs(depot, depot->completion.layer, newSlabCount);
  if (result != VDO_SUCCESS) {
    abandonNewSlabs(depot);
    return result;
  }

  depot->newSize      = newSize;
  depot->oldLastBlock = depot->lastBlock;
  depot->newLastBlock = newState.lastBlock;

  return VDO_SUCCESS;
}

/**
 * Finish registering new slabs now that all of the allocators have received
 * their new slabs.
 *
 * @param completion  The action completion
 **/
static void finishRegistration(VDOCompletion *completion)
{
  SlabDepot *depot = completion->parent;
  depot->resizeStepRequested = DEPOT_RESIZE_NONE;

  depot->slabCount = depot->newSlabCount;
  FREE(depot->slabs);
  depot->slabs = depot->newSlabs;
  depot->newSlabs = NULL;
  depot->newSlabCount = 0;

  // Launch any blocked lock release request.
  commitOldestSlabJournalTailBlocks(depot, depot->newReleaseRequest);
  completeCompletion(&depot->subTaskCompletion);
}

/**
 * Handle an error while registering (should be impossible).
 *
 * @param completion  The action completion
 **/
static void handleRegistrationError(VDOCompletion *completion)
{
  SlabDepot *depot = completion->parent;
  depot->resizeStepRequested = DEPOT_RESIZE_NONE;
  finishCompletion(&depot->subTaskCompletion, completion->result);
}

/**
 * Register new slabs.
 *
 * @param depot  The depot
 **/
static void registerNewSlabs(SlabDepot *depot) {
  launchOnAllAllocators(depot, registerNewSlabsForAllocator, depot,
                        finishRegistration, handleRegistrationError);
}

/**********************************************************************/
void useNewSlabs(SlabDepot     *depot,
                 VDOCompletion *parent,
                 VDOAction     *callback,
                 VDOAction     *errorHandler)
{
  ASSERT_LOG_ONLY(depot->newSlabs != NULL, "Must have new slabs to use");
  prepareCompletion(&depot->subTaskCompletion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  depot->resizeStepRequested = DEPOT_RESIZE_REGISTER_SLABS;
  if (!depot->lockReleaseActive) {
    registerNewSlabs(depot);
  }
}

/**
 * Finish now that all of the summary zones have been operated on.
 *
 * @param completion  The action completion
 **/
static void finishSummaryOperation(VDOCompletion *completion)
{
  SlabDepot *depot = completion->parent;
  depot->resizeStepRequested = DEPOT_RESIZE_NONE;

  // Launch any blocked lock release request.
  commitOldestSlabJournalTailBlocks(depot, depot->newReleaseRequest);
  completeCompletion(&depot->subTaskCompletion);
}

/**
 * Handle an error while operating on the summary.
 *
 * @param completion  The action completion
 **/
static void handleSummaryError(VDOCompletion *completion)
{
  SlabDepot *depot = completion->parent;
  depot->resizeStepRequested = DEPOT_RESIZE_NONE;
  finishCompletion(&depot->subTaskCompletion, completion->result);
}

/**
 * Suspend the slab summary.
 *
 * @param depot  The depot
 **/
static void suspendSummary(SlabDepot *depot)
{
  launchOnAllAllocators(depot, suspendSummaryZone, depot,
                        finishSummaryOperation, handleSummaryError);
}

/**********************************************************************/
void suspendSlabSummary(SlabDepot     *depot,
                        VDOCompletion *parent,
                        VDOAction     *callback,
                        VDOAction     *errorHandler)
{
  depot->resizeStepRequested = DEPOT_RESIZE_SUSPEND_SUMMARY;
  prepareCompletion(&depot->subTaskCompletion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  if (!depot->lockReleaseActive) {
    suspendSummary(depot);
  }
}

/**
 * Resume the slab summary.
 *
 * @param depot  The depot
 **/
static void resumeSummary(SlabDepot *depot)
{
  launchOnAllAllocators(depot, resumeSummaryZone, depot,
                        finishSummaryOperation, handleSummaryError);
}

/**********************************************************************/
void resumeSlabSummary(SlabDepot     *depot,
                       VDOCompletion *parent,
                       VDOAction     *callback,
                       VDOAction     *errorHandler)
{
  depot->resizeStepRequested = DEPOT_RESIZE_RESUME_SUMMARY;
  prepareCompletion(&depot->subTaskCompletion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  if (!depot->lockReleaseActive) {
    resumeSummary(depot);
  }
}

/**********************************************************************/
void saveSlabDepot(SlabDepot *depot, bool close, VDOCompletion *parent)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == depot->recoveryJournalThreadID),
                  "saveSlabDepot() called in journal zone");
  depot->saveRequested = close;
  prepareToFinishParent(&depot->saveCompletion, parent);
  AllocatorAction *action
    = (close ? closeBlockAllocator : saveBlockAllocatorForFullRebuild);

  if (!depot->lockReleaseActive) {
    applyToAllAllocators(depot, action, &depot->saveCompletion);
  }
}

/**
 * Check whether a request to save the depot, to resize, or to release more
 * locks, has come in while a lock release request was active. If so, do it.
 * This callback is registered in commitOldestSlabJournalTailBlocks().
 *
 * @param completion  The action completion
 **/
static void finishReleasingJournalLocks(VDOCompletion *completion)
{
  SlabDepot *depot = completion->parent;
  ASSERT_LOG_ONLY((getCallbackThreadID() == depot->recoveryJournalThreadID),
                  "finishReleasingJournalLocks() called in journal zone");

  depot->lockReleaseActive = false;
  if (depot->saveRequested) {
    applyToAllAllocators(depot, closeBlockAllocator, &depot->saveCompletion);
    return;
  }

  if (depot->resizeStepRequested == DEPOT_RESIZE_REGISTER_SLABS) {
    registerNewSlabs(depot);
    return;
  }

  if (depot->resizeStepRequested == DEPOT_RESIZE_RESUME_SUMMARY) {
    resumeSummary(depot);
    return;
  }

  if (depot->resizeStepRequested == DEPOT_RESIZE_SUSPEND_SUMMARY) {
    suspendSummary(depot);
    return;
  }

  commitOldestSlabJournalTailBlocks(depot, depot->newReleaseRequest);
}

/**********************************************************************/
void commitOldestSlabJournalTailBlocks(SlabDepot      *depot,
                                       SequenceNumber  recoveryBlockNumber)
{
  if (depot == NULL) {
    return;
  }

  ASSERT_LOG_ONLY((getCallbackThreadID() == depot->recoveryJournalThreadID),
                  "commitOldestSlabJournalTailBlocks() "
                  "called in journal zone");
  if (depot->saveRequested) {
    return;
  }

  depot->newReleaseRequest = recoveryBlockNumber;
  if (depot->lockReleaseActive
      || (depot->resizeStepRequested != DEPOT_RESIZE_NONE)
      || (depot->newReleaseRequest == depot->activeReleaseRequest)) {
    return;
  }

  depot->lockReleaseActive    = true;
  depot->activeReleaseRequest = depot->newReleaseRequest;
  launchOnAllAllocators(depot, releaseTailBlockLocks, depot,
                        finishReleasingJournalLocks,
                        finishReleasingJournalLocks);
}

/**********************************************************************/
const SlabConfig *getSlabConfig(const SlabDepot *depot)
{
  return &depot->slabConfig;
}

/**********************************************************************/
SlabSummary *getSlabSummary(const SlabDepot *depot)
{
  return depot->slabSummary;
}

/**********************************************************************/
SlabSummaryZone *getSlabSummaryForZone(const SlabDepot *depot, ZoneCount zone)
{
  if (depot->slabSummary == NULL) {
    return NULL;
  }
  return getSummaryForZone(depot->slabSummary, zone);
}

/**********************************************************************/
void scrubAllUnrecoveredSlabs(SlabDepot     *depot,
                              void          *parent,
                              VDOAction     *callback,
                              VDOAction     *errorHandler,
                              ThreadID       threadID,
                              VDOCompletion *launchParent)
{
  prepareCompletion(&depot->subTaskCompletion, callback, errorHandler,
                    threadID, parent);
  launchOnAllAllocators(depot, scrubAllUnrecoveredSlabsInZone, launchParent,
                        finishParentCallback, finishParentCallback);
}

/**********************************************************************/
void notifyZoneStoppedScrubbing(SlabDepot *depot)
{
  for (;;) {
    ZoneCount zonesScrubbing = atomicLoad32(&depot->zonesToScrub);
    if (!compareAndSwap32(&depot->zonesToScrub, zonesScrubbing,
                          zonesScrubbing - 1)) {
      // Someone else decremented the counter while we weren't looking.
      continue;
    }

    if (zonesScrubbing == 1) {
      // We're the last!
      completeCompletion(&depot->subTaskCompletion);
    }

    return;
  }
}

/**********************************************************************/
bool hasUnrecoveredSlabs(SlabDepot *depot)
{
  return (atomicLoad32(&depot->zonesToScrub) > 0);
}

/**********************************************************************/
BlockCount getNewDepotSize(const SlabDepot *depot)
{
  return (depot->newSlabs == NULL) ? 0 : depot->newSize;
}

/**********************************************************************/
bool areEquivalentDepots(SlabDepot *depotA, SlabDepot *depotB)
{
  if ((depotA->firstBlock       != depotB->firstBlock)
      || (depotA->lastBlock     != depotB->lastBlock)
      || (depotA->slabCount     != depotB->slabCount)
      || (depotA->slabSizeShift != depotB->slabSizeShift)
      || (getDepotAllocatedBlocks(depotA)
          != getDepotAllocatedBlocks(depotB))) {
    return false;
  }

  for (size_t i = 0; i < depotA->slabCount; i++) {
    Slab *slabA = depotA->slabs[i];
    Slab *slabB = depotB->slabs[i];
    if ((slabA->start  != slabB->start)
        || (slabA->end != slabB->end)
        || !areEquivalentReferenceCounters(slabA->referenceCounts,
                                           slabB->referenceCounts)) {
      return false;
    }
  }

  return true;
}

/**********************************************************************/
void allocateFromLastSlab(SlabDepot *depot)
{
  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    allocateFromAllocatorLastSlab(depot->allocators[zone]);
  }
}

/**********************************************************************/
BlockAllocatorStatistics
getDepotBlockAllocatorStatistics(const SlabDepot *depot)
{
  BlockAllocatorStatistics totals;
  memset(&totals, 0, sizeof(totals));

  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    BlockAllocator *allocator = depot->allocators[zone];
    BlockAllocatorStatistics stats = getBlockAllocatorStatistics(allocator);
    totals.slabCount     += stats.slabCount;
    totals.slabsOpened   += stats.slabsOpened;
    totals.slabsReopened += stats.slabsReopened;
  }

  return totals;
}

/**********************************************************************/
RefCountsStatistics getDepotRefCountsStatistics(const SlabDepot *depot)
{
  RefCountsStatistics depotStats;
  memset(&depotStats, 0, sizeof(depotStats));

  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    BlockAllocator *allocator = depot->allocators[zone];
    RefCountsStatistics stats = getRefCountsStatistics(allocator);
    depotStats.blocksWritten += stats.blocksWritten;
  }

  return depotStats;
}

/**********************************************************************/
SlabJournalStatistics getDepotSlabJournalStatistics(const SlabDepot *depot)
{
  SlabJournalStatistics depotStats;
  memset(&depotStats, 0, sizeof(depotStats));

  for (ZoneCount zone = 0; zone < depot->zoneCount; zone++) {
    BlockAllocator *allocator = depot->allocators[zone];
    SlabJournalStatistics stats = getSlabJournalStatistics(allocator);
    depotStats.diskFullCount += stats.diskFullCount;
    depotStats.flushCount    += stats.flushCount;
    depotStats.blockedCount  += stats.blockedCount;
    depotStats.blocksWritten += stats.blocksWritten;
    depotStats.tailBusyCount += stats.tailBusyCount;
  }

  return depotStats;
}

/**********************************************************************/
void dumpSlabDepot(const SlabDepot *depot)
{
  logInfo("Slab Depot");
  logInfo("  zoneCount=%u oldZoneCount=%u slabCount=%" PRIu32
          " activeReleaseRequest=%" PRIu64 " newReleaseRequest=%" PRIu64,
          (unsigned int) depot->zoneCount, (unsigned int) depot->oldZoneCount,
          depot->slabCount, depot->activeReleaseRequest,
          depot->newReleaseRequest);
}
