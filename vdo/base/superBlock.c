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
 * $Id: //eng/vdo-releases/magnesium-rhel7.6/src/c++/vdo/base/superBlock.c#1 $
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

static const Header *CURRENT_SUPER_BLOCK_HEADER
  = &SUPER_BLOCK_HEADER_12_0;

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
  freeBuffer(&superBlock->componentBuffer);
  freeVIO(&superBlock->vio);
  FREE(superBlock->encodedSuperBlock);
  FREE(superBlock);
  *superBlockPtr = NULL;
}

/**
 * Copy bytes at a specified offset in the destination buffer.
 *
 * @param [out] destination  The buffer to copy into
 * @param [in]  source       The source data to copy
 * @param [in]  length       The length of the data to copy
 * @param [in]  offset       The offset into the destination buffer at which to
 *                           start the copy
 *
 * @return The offset of the first uncopied byte
 **/
static size_t putBytesAt(byte       *destination,
                         const void *source,
                         size_t      length,
                         size_t      offset)
{
  memcpy(destination + offset, source, length);
  return offset + length;
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
  size_t componentDataSize = contentLength(superBlock->componentBuffer);
  Header header            = *CURRENT_SUPER_BLOCK_HEADER;
  header.size
    = sizeof(ReleaseVersionNumber) + componentDataSize + CHECKSUM_SIZE;

  // Encode the header.
  size_t offset = putBytesAt(superBlock->encodedSuperBlock, &header,
                             ENCODED_HEADER_SIZE, 0);

  // Encode the release version.
  ReleaseVersionNumber version = CURRENT_RELEASE_VERSION_NUMBER;
  offset = putBytesAt(superBlock->encodedSuperBlock, &version,
                      sizeof(ReleaseVersionNumber), offset);

  // Encode the component data.
  int result = getBytesFromBuffer(superBlock->componentBuffer,
                                  componentDataSize,
                                  superBlock->encodedSuperBlock + offset);
  if (result != VDO_SUCCESS) {
    return result;
  }

  offset += componentDataSize;

  // Compute and encode the check sum.
  CRC32Checksum checksum = layer->updateCRC32(INITIAL_CHECKSUM,
                                              superBlock->encodedSuperBlock,
                                              offset);
  putBytesAt(superBlock->encodedSuperBlock, &checksum,
             sizeof(CRC32Checksum), offset);
  return VDO_SUCCESS;
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
 * Copy bytes from a specified offset in the destination buffer.
 *
 * @param [out] destination   The buffer to copy into
 * @param [in]  source        The source data to copy
 * @param [in]  length        The length of the data to copy
 * @param [in]  offset        The offset into the source buffer at which to
 *                            start the copy
 *
 * @return The offset of the first uncopied byte
 **/
static size_t getBytesFrom(void       *destination,
                           const byte *source,
                           size_t      length,
                           size_t      offset)
{
  memcpy(destination, source + offset, length);
  return offset + length;
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
  Header header;
  size_t offset = getBytesFrom(&header, superBlock->encodedSuperBlock,
                               ENCODED_HEADER_SIZE, 0);

  int result = validateHeader(CURRENT_SUPER_BLOCK_HEADER, &header,
                              false, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // The entire SuperBlock encoding must have fit in a single block.
  // XXX shouldn't this be checked against the sector size now?
  if (VDO_BLOCK_SIZE < (header.size + ENCODED_HEADER_SIZE)) {
    return VDO_BLOCK_SIZE_TOO_SMALL;
  }

  // Decode and store the release version number. It will be checked when the
  // VDO master version is decoded and validated.
  offset = getBytesFrom(&superBlock->loadedReleaseVersion,
                        superBlock->encodedSuperBlock,
                        sizeof(ReleaseVersionNumber), offset);

  size_t componentDataSize
    = header.size - (SUPER_BLOCK_FIXED_SIZE - ENCODED_HEADER_SIZE);
  result = putBytes(superBlock->componentBuffer, componentDataSize,
                    superBlock->encodedSuperBlock + offset);
  if (result != VDO_SUCCESS) {
    return result;
  }
  offset += componentDataSize;

  CRC32Checksum checksum = layer->updateCRC32(INITIAL_CHECKSUM,
                                              superBlock->encodedSuperBlock,
                                              offset);
  CRC32Checksum savedChecksum;
  getBytesFrom(&savedChecksum, superBlock->encodedSuperBlock,
               sizeof(CRC32Checksum), offset);
  if (checksum != savedChecksum) {
    result = skipForward(superBlock->componentBuffer,
                         contentLength(superBlock->componentBuffer));
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "Impossible buffer error");
    }
    return VDO_CHECKSUM_MISMATCH;
  }

  return VDO_SUCCESS;
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
