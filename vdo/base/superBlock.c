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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/superBlock.c#3 $
 */

#include "superBlock.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "constants.h"
#include "header.h"
#include "releaseVersions.h"
#include "statusCodes.h"
#include "types.h"
#include "vio.h"

struct superBlock {
  /** The parent for asynchronous load and save operations */
  VDOCompletion           *parent;
  /** The VIO for reading and writing the super block to disk */
  VIO                     *vio;
  /** The buffer for encoding and decoding component data */
  Buffer                  *componentBuffer;
  /**
   * A sector-sized buffer wrapping the first sector of encodedSuperBlock, for
   * encoding and decoding the entire super block.
   **/
  Buffer                  *blockBuffer;
  /** A 1-block buffer holding the encoded on-disk super block */
  byte                    *encodedSuperBlock;
  /** The release version number loaded from the volume */
  ReleaseVersionNumber     loadedReleaseVersion;
};

enum {
  SUPER_BLOCK_FIXED_SIZE
    = ENCODED_HEADER_SIZE + sizeof(ReleaseVersionNumber) + CHECKSUM_SIZE,
  MAX_COMPONENT_DATA_SIZE = VDO_SECTOR_SIZE - SUPER_BLOCK_FIXED_SIZE,
};

static const Header SUPER_BLOCK_HEADER_12_0 = {
  .id = SUPER_BLOCK,
  .version = {
    .majorVersion = 12,
    .minorVersion = 0,
  },

  // This is the minimum size, if the super block contains no components.
  .size = SUPER_BLOCK_FIXED_SIZE - ENCODED_HEADER_SIZE,
};

/**
 * Allocate a super block. Callers must free the allocated super block even
 * on error.
 *
 * @param layer  The physical layer which holds the super block on disk
 * @param superBlockPtr  A pointer to hold the new super block
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int allocateSuperBlock(PhysicalLayer *layer, SuperBlock **superBlockPtr)
{
  int result = ALLOCATE(1, SuperBlock, __func__, superBlockPtr);
  if (result != UDS_SUCCESS) {
    return result;
  }

  SuperBlock *superBlock = *superBlockPtr;
  result = makeBuffer(MAX_COMPONENT_DATA_SIZE, &superBlock->componentBuffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE,
                                   "encoded super block",
                                   (char **) &superBlock->encodedSuperBlock);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Even though the buffer is a full block, to avoid the potential corruption
  // from a torn write, the entire encoding must fit in the first sector.
  result = wrapBuffer(superBlock->encodedSuperBlock, VDO_SECTOR_SIZE, 0,
                      &superBlock->blockBuffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (layer->createMetadataVIO == NULL) {
    return VDO_SUCCESS;
  }

  return createVIO(layer, VIO_TYPE_SUPER_BLOCK, VIO_PRIORITY_METADATA,
                   superBlock, (char *) superBlock->encodedSuperBlock,
                   &superBlock->vio);
}

/**********************************************************************/
int makeSuperBlock(PhysicalLayer *layer, SuperBlock **superBlockPtr)
{
  SuperBlock *superBlock;
  int         result = allocateSuperBlock(layer, &superBlock);
  if (result != VDO_SUCCESS) {
    freeSuperBlock(&superBlock);
    return result;
  }

  // For a new super block, use the current release.
  superBlock->loadedReleaseVersion = CURRENT_RELEASE_VERSION_NUMBER;
  *superBlockPtr = superBlock;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeSuperBlock(SuperBlock **superBlockPtr)
{
  if (*superBlockPtr == NULL) {
    return;
  }

  SuperBlock *superBlock = *superBlockPtr;
  freeBuffer(&superBlock->blockBuffer);
  freeBuffer(&superBlock->componentBuffer);
  freeVIO(&superBlock->vio);
  FREE(superBlock->encodedSuperBlock);
  FREE(superBlock);
  *superBlockPtr = NULL;
}

/**
 * Encode a super block into its on-disk representation.
 *
 * @param layer       The physical layer which implements the checksum
 * @param superBlock  The super block to encode
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int encodeSuperBlock(PhysicalLayer *layer, SuperBlock *superBlock)
{
  Buffer *buffer = superBlock->blockBuffer;
  int     result = resetBufferEnd(buffer, 0);
  if (result != VDO_SUCCESS) {
    return result;
  }

  size_t componentDataSize = contentLength(superBlock->componentBuffer);

  // Encode the header.
  Header header = SUPER_BLOCK_HEADER_12_0;
  header.size += componentDataSize;
  result = encodeHeader(&header, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Encode the loaded release version.
  result = putUInt32LEIntoBuffer(buffer, superBlock->loadedReleaseVersion);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Copy the already-encoded component data.
  result = putBytes(buffer, componentDataSize,
                    getBufferContents(superBlock->componentBuffer));
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Compute and encode the checksum.
  CRC32Checksum checksum = layer->updateCRC32(INITIAL_CHECKSUM,
                                              superBlock->encodedSuperBlock,
                                              contentLength(buffer));
  result = putUInt32LEIntoBuffer(buffer, checksum);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
int saveSuperBlock(PhysicalLayer       *layer,
                   SuperBlock          *superBlock,
                   PhysicalBlockNumber  superBlockOffset)
{
  int result = encodeSuperBlock(layer, superBlock);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return layer->writer(layer, superBlockOffset, 1,
                       (char *) superBlock->encodedSuperBlock, NULL);
}

/**
 * Finish the parent of a super block load or save operation. This
 * callback is registered in saveSuperBlockAsync() and loadSuperBlockAsync.
 *
 * @param completion  The super block VIO
 **/
static void finishSuperBlockParent(VDOCompletion *completion)
{
  SuperBlock    *superBlock = completion->parent;
  VDOCompletion *parent     = superBlock->parent;
  superBlock->parent        = NULL;
  finishCompletion(parent, completion->result);
}

/**********************************************************************/
void saveSuperBlockAsync(SuperBlock          *superBlock,
                         PhysicalBlockNumber  superBlockOffset,
                         VDOCompletion       *parent)
{
  if (superBlock->parent != NULL) {
    finishCompletion(parent, VDO_COMPONENT_BUSY);
    return;
  }

  PhysicalLayer *layer = parent->layer;
  int result = encodeSuperBlock(layer, superBlock);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  superBlock->parent                           = parent;
  superBlock->vio->completion.callbackThreadID = parent->callbackThreadID;
  launchWriteMetadataVIOWithFlush(superBlock->vio, superBlockOffset,
                                  finishSuperBlockParent,
                                  finishSuperBlockParent, true, true);
}

/**
 * Decode a super block from its on-disk representation.
 *
 * @param layer       The physical layer which implements the checksum
 * @param superBlock  The super block to decode
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int decodeSuperBlock(PhysicalLayer *layer, SuperBlock *superBlock)
{
  // Reset the block buffer to start decoding the entire first sector.
  Buffer *buffer = superBlock->blockBuffer;
  clearBuffer(buffer);

  // Decode and validate the header.
  Header header;
  int result = decodeHeader(buffer, &header);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = validateHeader(&SUPER_BLOCK_HEADER_12_0, &header, false, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (header.size > contentLength(buffer)) {
    // We can't check release version or checksum until we know the content
    // size, so we have to assume a version mismatch on unexpected values.
    return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                                   "super block contents too large: %zu",
                                   header.size);
  }

  // Restrict the buffer to the actual payload bytes that remain.
  result = resetBufferEnd(buffer, uncompactedAmount(buffer) + header.size);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Decode and store the release version number. It will be checked when the
  // VDO master version is decoded and validated.
  result = getUInt32LEFromBuffer(buffer, &superBlock->loadedReleaseVersion);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // The component data is all the rest, except for the checksum.
  size_t componentDataSize = contentLength(buffer) - sizeof(CRC32Checksum);
  result = putBuffer(superBlock->componentBuffer, buffer, componentDataSize);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Checksum everything up to but not including the saved checksum itself.
  CRC32Checksum checksum = layer->updateCRC32(INITIAL_CHECKSUM,
                                              superBlock->encodedSuperBlock,
                                              uncompactedAmount(buffer));

  // Decode and verify the saved checksum.
  CRC32Checksum savedChecksum;
  result = getUInt32LEFromBuffer(buffer, &savedChecksum);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ASSERT(contentLength(buffer) == 0,
                  "must have decoded entire superblock payload");
  if (result != VDO_SUCCESS) {
    return result;
  }

  return ((checksum != savedChecksum) ? VDO_CHECKSUM_MISMATCH : VDO_SUCCESS);
}

/**********************************************************************/
int loadSuperBlock(PhysicalLayer        *layer,
                   PhysicalBlockNumber   superBlockOffset,
                   SuperBlock          **superBlockPtr)
{
  SuperBlock *superBlock = NULL;
  int         result     = allocateSuperBlock(layer, &superBlock);
  if (result != VDO_SUCCESS) {
    freeSuperBlock(&superBlock);
    return result;
  }

  result = layer->reader(layer, superBlockOffset, 1,
                         (char *) superBlock->encodedSuperBlock, NULL);
  if (result != VDO_SUCCESS) {
    freeSuperBlock(&superBlock);
    return result;
  }

  result = decodeSuperBlock(layer, superBlock);
  if (result != VDO_SUCCESS) {
    freeSuperBlock(&superBlock);
    return result;
  }

  *superBlockPtr = superBlock;
  return result;
}

/**
 * Continue after loading the super block. This callback is registered
 * in loadSuperBlockAsync().
 *
 * @param completion  The super block VIO
 **/
static void finishReadingSuperBlock(VDOCompletion *completion)
{
  SuperBlock    *superBlock = completion->parent;
  VDOCompletion *parent     = superBlock->parent;
  superBlock->parent        = NULL;
  finishCompletion(parent, decodeSuperBlock(completion->layer, superBlock));
}

/**********************************************************************/
void loadSuperBlockAsync(VDOCompletion        *parent,
                         PhysicalBlockNumber   superBlockOffset,
                         SuperBlock          **superBlockPtr)
{
  PhysicalLayer *layer      = parent->layer;
  SuperBlock    *superBlock = NULL;
  int            result     = allocateSuperBlock(layer, &superBlock);
  if (result != VDO_SUCCESS) {
    freeSuperBlock(&superBlock);
    finishCompletion(parent, result);
    return;
  }

  *superBlockPtr = superBlock;

  superBlock->parent                           = parent;
  superBlock->vio->completion.callbackThreadID = parent->callbackThreadID;
  launchReadMetadataVIO(superBlock->vio, superBlockOffset,
                        finishReadingSuperBlock, finishSuperBlockParent);
}

/**********************************************************************/
Buffer *getComponentBuffer(SuperBlock *superBlock)
{
  return superBlock->componentBuffer;
}

/**********************************************************************/
ReleaseVersionNumber getLoadedReleaseVersion(const SuperBlock *superBlock)
{
  return superBlock->loadedReleaseVersion;
}

/**********************************************************************/
size_t getFixedSuperBlockSize(void)
{
  return SUPER_BLOCK_FIXED_SIZE;
}
