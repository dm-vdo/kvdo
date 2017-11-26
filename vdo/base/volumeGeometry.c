/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/volumeGeometry.c#2 $
 */

#include "volumeGeometry.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "physicalLayer.h"
#include "releaseVersions.h"
#include "statusCodes.h"
#include "superBlock.h"
#include "types.h"

enum { GEOMETRY_BLOCK_LOCATION = 0 };

static const Header GEOMETRY_BLOCK_HEADER_2_0 = {
  .id = GEOMETRY_BLOCK,
  .version = {
    .majorVersion = 2,
    .minorVersion = 0,
  },

  .size = (ENCODED_HEADER_SIZE + sizeof(ReleaseVersionNumber)
           + sizeof(VolumeGeometry) + sizeof(CRC32Checksum)),
};

static const Header *CURRENT_GEOMETRY_BLOCK_HEADER
  = &GEOMETRY_BLOCK_HEADER_2_0;

typedef struct {
  Header               header;
  ReleaseVersionNumber version;
  VolumeGeometry       geometry;
  CRC32Checksum        checksum;
} __attribute__((packed)) GeometryBlock;

/************************************************************************/
int loadVolumeGeometry(PhysicalLayer *layer, VolumeGeometry **geometryPtr)
{
  int result = ASSERT(layer->reader != NULL, "Layer must have a sync reader");
  if (result != VDO_SUCCESS) {
    return result;
  }

  char *block;
  result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block",
                                   &block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = layer->reader(layer, GEOMETRY_BLOCK_LOCATION, 1, block, NULL);
  if (result != VDO_SUCCESS) {
    FREE(block);
    return result;
  }

  GeometryBlock *geometryBlock = (GeometryBlock *) block;

  result = validateHeader(CURRENT_GEOMETRY_BLOCK_HEADER,
                          &geometryBlock->header, true, __func__);
  if (result != VDO_SUCCESS) {
    FREE(block);
    return result;
  }

  // Decode the release version number. It must be the present.
  if (geometryBlock->version != CURRENT_RELEASE_VERSION_NUMBER) {
    logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                            "release version %d requires an upgrade",
                            geometryBlock->version);
    FREE(block);
    return VDO_UNSUPPORTED_VERSION;
  }

  VolumeGeometry *geometry;
  result = ALLOCATE(1, VolumeGeometry, "geometry", &geometry);
  if (result != VDO_SUCCESS) {
    FREE(block);
    return result;
  }

  memcpy(geometry, &geometryBlock->geometry, sizeof(VolumeGeometry));

  size_t checksummedBytes = sizeof(GeometryBlock) - sizeof(CRC32Checksum);
  CRC32Checksum checksum = layer->updateCRC32(INITIAL_CHECKSUM,
                                              (byte *) block,
                                              checksummedBytes);
  if (checksum != geometryBlock->checksum) {
    FREE(geometry);
    FREE(block);
    return VDO_CHECKSUM_MISMATCH;
  }

  *geometryPtr = geometry;
  FREE(block);
  return VDO_SUCCESS;
}

/**
 * Compute the index size in blocks from the IndexConfig.
 *
 * @param [in]  indexConfig     The index config
 * @param [out] indexBlocksPtr  A pointer to return the index size in blocks
 *
 * @return VDO_SUCCESS or an error
 **/
static int computeIndexBlocks(IndexConfig *indexConfig,
                              BlockCount  *indexBlocksPtr)
{
  UdsConfiguration udsConfiguration;
  int result = indexConfigToUdsConfiguration(indexConfig, &udsConfiguration);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "error creating index config");
  }

  uint64_t indexBytes;
  result = udsComputeIndexSize(udsConfiguration, 0, &indexBytes);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "error computing index size");
  }

  udsFreeConfiguration(udsConfiguration);

  BlockCount indexBlocks = indexBytes / VDO_BLOCK_SIZE;
  if ((((uint64_t) indexBlocks) * VDO_BLOCK_SIZE) != indexBytes) {
    return logErrorWithStringError(VDO_PARAMETER_MISMATCH, "index size must be a"
                                   " multiple of block size %d",
                                   VDO_BLOCK_SIZE);
  }

  *indexBlocksPtr = indexBlocks;
  return VDO_SUCCESS;
}

/************************************************************************/
int writeVolumeGeometry(PhysicalLayer *layer,
                        Nonce          nonce,
                        IndexConfig   *indexConfig)
{
  char *block;
  int result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block",
                                       &block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  GeometryBlock *geometryBlock = (GeometryBlock *) block;

  BlockCount indexSize = 0;
  if (indexConfig != NULL) {
    memcpy(&geometryBlock->geometry.indexConfig, indexConfig,
           sizeof(*indexConfig));

    result = computeIndexBlocks(indexConfig, &indexSize);
    if (result != VDO_SUCCESS) {
      return result;
    }
  } else {
    memset(&geometryBlock->geometry.indexConfig, 0, sizeof(IndexConfig));
    indexSize = 0;
  }

  geometryBlock->header = *CURRENT_GEOMETRY_BLOCK_HEADER;
  geometryBlock->version = CURRENT_RELEASE_VERSION_NUMBER;

  geometryBlock->geometry.nonce = nonce;
  geometryBlock->geometry.partitions[INDEX_REGION] = (VolumePartition) {
    .id         = INDEX_REGION,
    .startBlock = 1,
  };
  geometryBlock->geometry.partitions[DATA_REGION] = (VolumePartition) {
    .id         = DATA_REGION,
    .startBlock = 1 + indexSize,
  };

  // Checksum everything.
  size_t checksummedBytes = sizeof(GeometryBlock) - sizeof(CRC32Checksum);
  geometryBlock->checksum = layer->updateCRC32(INITIAL_CHECKSUM,
                                               (byte *) block,
                                               checksummedBytes);

  // Write it.
  result = layer->writer(layer, GEOMETRY_BLOCK_LOCATION, 1, block, NULL);
  FREE(block);
  return result;
}

/************************************************************************/
int indexConfigToUdsConfiguration(IndexConfig      *indexConfig,
                                  UdsConfiguration *udsConfigPtr)
{
  UdsConfiguration udsConfiguration;
  int result = udsInitializeConfiguration(&udsConfiguration, indexConfig->mem);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "error initializing configuration");
  }

  udsConfigurationSetSparse(udsConfiguration, indexConfig->sparse);

  uint32_t cfreq = indexConfig->checkpointFrequency;
  result = udsConfigurationSetCheckpointFrequency(udsConfiguration, cfreq);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "error setting checkpoint"
                                   " frequency");
  }

  *udsConfigPtr = udsConfiguration;
  return VDO_SUCCESS;
}
