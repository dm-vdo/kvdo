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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/volumeGeometry.c#1 $
 */

#include "volumeGeometry.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "physicalLayer.h"
#include "releaseVersions.h"
#include "statusCodes.h"
#include "superBlock.h"
#include "types.h"

enum { GEOMETRY_BLOCK_LOCATION = 0 };

static const Header GEOMETRY_BLOCK_HEADER_1_0 = {
  .id = GEOMETRY_BLOCK,
  .version = {
    .majorVersion = 1,
    .minorVersion = 0,
  },

  .size = (ENCODED_HEADER_SIZE + sizeof(ReleaseVersionNumber)
           + sizeof(VolumeGeometry) + sizeof(CRC32Checksum)),
};

static const Header *CURRENT_GEOMETRY_BLOCK_HEADER
  = &GEOMETRY_BLOCK_HEADER_1_0;

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

/************************************************************************/
int writeVolumeGeometry(PhysicalLayer *layer,
                        Nonce          nonce,
                        BlockCount     indexSize)
{
  char *block;
  int result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block",
                                       &block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  GeometryBlock *geometryBlock = (GeometryBlock *) block;

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
