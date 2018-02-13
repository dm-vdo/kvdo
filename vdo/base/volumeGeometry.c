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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/volumeGeometry.c#7 $
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

enum {
  GEOMETRY_BLOCK_LOCATION = 0,
  MAGIC_NUMBER_SIZE       = 8,
};

typedef struct {
  char                 magicNumber[MAGIC_NUMBER_SIZE];
  Header               header;
  ReleaseVersionNumber version;
  VolumeGeometry       geometry;
  CRC32Checksum        checksum;
} __attribute__((packed)) GeometryBlock;

static const Header GEOMETRY_BLOCK_HEADER_4_0 = {
  .id = GEOMETRY_BLOCK,
  .version = {
    .majorVersion = 4,
    .minorVersion = 0,
  },

  .size = sizeof(GeometryBlock),
};

static const Header *CURRENT_GEOMETRY_BLOCK_HEADER
  = &GEOMETRY_BLOCK_HEADER_4_0;

static const char MAGIC_NUMBER[MAGIC_NUMBER_SIZE + 1] = "dmvdo001";

/**********************************************************************/
int loadVolumeGeometry(PhysicalLayer *layer, VolumeGeometry *geometry)
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
  if (memcmp(geometryBlock->magicNumber, MAGIC_NUMBER,
             MAGIC_NUMBER_SIZE) != 0) {
    FREE(block);
    return VDO_BAD_MAGIC;
  }

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

  size_t checksummedBytes = sizeof(GeometryBlock) - sizeof(CRC32Checksum);
  CRC32Checksum checksum = layer->updateCRC32(INITIAL_CHECKSUM,
                                              (byte *) block,
                                              checksummedBytes);
  if (checksum != geometryBlock->checksum) {
    FREE(block);
    return VDO_CHECKSUM_MISMATCH;
  }

  memcpy(geometry, &geometryBlock->geometry, sizeof(VolumeGeometry));
  FREE(block);
  return VDO_SUCCESS;
}

/************************************************************************/
int computeIndexBlocks(IndexConfig *indexConfig, BlockCount *indexBlocksPtr)
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

/**********************************************************************/
int initializeVolumeGeometry(Nonce           nonce,
                             UUID            uuid,
                             IndexConfig    *indexConfig,
                             VolumeGeometry *geometry)
{
  BlockCount indexSize = 0;
  if (indexConfig != NULL) {
    int result = computeIndexBlocks(indexConfig, &indexSize);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  geometry->nonce = nonce;
  memcpy(geometry->uuid, uuid, sizeof(UUID));
  geometry->partitions[INDEX_REGION] = (VolumePartition) {
    .id         = INDEX_REGION,
    .startBlock = 1,
  };
  geometry->partitions[DATA_REGION] = (VolumePartition) {
    .id         = DATA_REGION,
    .startBlock = 1 + indexSize,
  };

  if (indexSize > 0) {
    memcpy(&geometry->indexConfig, indexConfig, sizeof(IndexConfig));
  } else {
    memset(&geometry->indexConfig, 0, sizeof(IndexConfig));
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int clearVolumeGeometry(PhysicalLayer *layer)
{
  char *block;
  int result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block",
                                       &block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = layer->writer(layer, GEOMETRY_BLOCK_LOCATION, 1, block, NULL);
  FREE(block);
  return result;
}

/**********************************************************************/
int writeVolumeGeometry(PhysicalLayer *layer, VolumeGeometry *geometry)
{
  char *block;
  int result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block",
                                       &block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  GeometryBlock *geometryBlock = (GeometryBlock *) block;
  memcpy(geometryBlock->magicNumber, &MAGIC_NUMBER, MAGIC_NUMBER_SIZE);
  geometryBlock->header   = *CURRENT_GEOMETRY_BLOCK_HEADER;
  geometryBlock->version  = CURRENT_RELEASE_VERSION_NUMBER;
  geometryBlock->geometry = *geometry;

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
    udsFreeConfiguration(udsConfiguration);
    return logErrorWithStringError(result, "error setting checkpoint"
                                   " frequency");
  }

  *udsConfigPtr = udsConfiguration;
  return VDO_SUCCESS;
}
