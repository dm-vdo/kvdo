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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/volumeGeometry.c#7 $
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
#include "types.h"

enum {
  GEOMETRY_BLOCK_LOCATION = 0,
  MAGIC_NUMBER_SIZE       = 8,
};

typedef struct {
  char            magicNumber[MAGIC_NUMBER_SIZE];
  Header          header;
  VolumeGeometry  geometry;
  CRC32Checksum   checksum;
} __attribute__((packed)) GeometryBlock;

static const Header GEOMETRY_BLOCK_HEADER_4_0 = {
  .id = GEOMETRY_BLOCK,
  .version = {
    .majorVersion = 4,
    .minorVersion = 0,
  },
  // Note: this size isn't just the payload size following the header, like it
  // is everywhere else in VDO.
  .size = sizeof(GeometryBlock),
};

static const byte MAGIC_NUMBER[MAGIC_NUMBER_SIZE + 1] = "dmvdo001";

static const ReleaseVersionNumber COMPATIBLE_RELEASE_VERSIONS[] = {
  MAGNESIUM_RELEASE_VERSION_NUMBER,
};

/**
 * Determine whether the supplied release version can be understood by
 * the VDO code.
 *
 * @param version  The release version number to check
 *
 * @return <code>True</code> if the given version can be loaded.
 **/
static inline bool isLoadableReleaseVersion(ReleaseVersionNumber version)
{
  if (version == CURRENT_RELEASE_VERSION_NUMBER) {
    return true;
  }

  for (unsigned int i = 0; i < COUNT_OF(COMPATIBLE_RELEASE_VERSIONS); i++) {
    if (version == COMPATIBLE_RELEASE_VERSIONS[i]) {
      return true;
    }
  }

  return false;
}

/**
 * Decode the on-disk representation of an index configuration from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The structure to receive the decoded fields
 *
 * @return UDS_SUCCESS or an error
 **/
static int decodeIndexConfig(Buffer *buffer, IndexConfig *config)
{
  int result = getUInt32LEFromBuffer(buffer, &config->mem);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = getUInt32LEFromBuffer(buffer, &config->checkpointFrequency);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return getBoolean(buffer, &config->sparse);
}

/**
 * Encode the on-disk representation of an index configuration into a buffer.
 *
 * @param config  The index configuration to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error
 **/
static int encodeIndexConfig(const IndexConfig *config, Buffer *buffer)
{
  int result = putUInt32LEIntoBuffer(buffer, config->mem);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = putUInt32LEIntoBuffer(buffer, config->checkpointFrequency);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return putBoolean(buffer, config->sparse);
}

/**
 * Decode the on-disk representation of a volume region from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param region  The structure to receive the decoded fields
 *
 * @return UDS_SUCCESS or an error
 **/
static int decodeVolumeRegion(Buffer *buffer, VolumeRegion *region)
{
  int result = getUInt32LEFromBuffer(buffer, &region->id);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return getUInt64LEFromBuffer(buffer, &region->startBlock);
}

/**
 * Encode the on-disk representation of a volume region into a buffer.
 *
 * @param region  The region to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error
 **/
static int encodeVolumeRegion(const VolumeRegion *region, Buffer *buffer)
{
  int result = putUInt32LEIntoBuffer(buffer, region->id);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return putUInt64LEIntoBuffer(buffer, region->startBlock);
}

/**
 * Decode the on-disk representation of a volume geometry from a buffer.
 *
 * @param buffer    A buffer positioned at the start of the encoding
 * @param geometry  The structure to receive the decoded fields
 *
 * @return UDS_SUCCESS or an error
 **/
static int decodeVolumeGeometry(Buffer *buffer, VolumeGeometry *geometry)
{
  int result = getUInt32LEFromBuffer(buffer, &geometry->releaseVersion);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &geometry->nonce);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = getBytesFromBuffer(buffer, sizeof(UUID), geometry->uuid);
  if (result != VDO_SUCCESS) {
    return result;
  }

  for (VolumeRegionID id = 0; id < VOLUME_REGION_COUNT; id++) {
    result = decodeVolumeRegion(buffer, &geometry->regions[id]);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return decodeIndexConfig(buffer, &geometry->indexConfig);
}

/**
 * Encode the on-disk representation of a volume geometry into a buffer.
 *
 * @param geometry  The geometry to encode
 * @param buffer    A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error
 **/
static int encodeVolumeGeometry(const VolumeGeometry *geometry, Buffer *buffer)
{
  int result = putUInt32LEIntoBuffer(buffer, geometry->releaseVersion);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, geometry->nonce);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = putBytes(buffer, sizeof(UUID), geometry->uuid);
  if (result != VDO_SUCCESS) {
    return result;
  }

  for (VolumeRegionID id = 0; id < VOLUME_REGION_COUNT; id++) {
    result = encodeVolumeRegion(&geometry->regions[id], buffer);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return encodeIndexConfig(&geometry->indexConfig, buffer);
}

/**
 * Decode the on-disk representation of a geometry block, up to but not
 * including the checksum, from a buffer.
 *
 * @param buffer    A buffer positioned at the start of the block
 * @param geometry  The structure to receive the decoded volume geometry fields
 *
 * @return UDS_SUCCESS or an error
 **/
static int decodeGeometryBlock(Buffer *buffer, VolumeGeometry *geometry)
{
  if (!hasSameBytes(buffer, MAGIC_NUMBER, MAGIC_NUMBER_SIZE)) {
    return VDO_BAD_MAGIC;
  }

  int result = skipForward(buffer, MAGIC_NUMBER_SIZE);
  if (result != VDO_SUCCESS) {
    return result;
  }

  Header header;
  result = decodeHeader(buffer, &header);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = validateHeader(&GEOMETRY_BLOCK_HEADER_4_0, &header, true, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeVolumeGeometry(buffer, geometry);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Leave the CRC for the caller to decode and verify.
  return ASSERT(header.size
                == (uncompactedAmount(buffer) + sizeof(CRC32Checksum)),
                "should have decoded up to the geometry checksum");
}

/**
 * Encode the on-disk representation of a geometry block, up to but not
 * including the checksum, into a buffer.
 *
 * @param geometry  The volume geometry to encode into the block
 * @param buffer    A buffer positioned at the start of the block
 *
 * @return UDS_SUCCESS or an error
 **/
static int encodeGeometryBlock(const VolumeGeometry *geometry, Buffer *buffer)
{
  int result = putBytes(buffer, MAGIC_NUMBER_SIZE, MAGIC_NUMBER);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encodeHeader(&GEOMETRY_BLOCK_HEADER_4_0, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encodeVolumeGeometry(geometry, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Leave the CRC for the caller to compute and encode.
  return ASSERT(GEOMETRY_BLOCK_HEADER_4_0.size
                == (contentLength(buffer) + sizeof(CRC32Checksum)),
                "should have decoded up to the geometry checksum");
}

/**
 * Allocate a block-size buffer to read the geometry from the physical layer,
 * read the block, and return the buffer.
 *
 * @param [in]  layer     The physical layer containing the block to read
 * @param [out] blockPtr  A pointer to receive the allocated buffer
 *
 * @return VDO_SUCCESS or an error code
 **/
static int readGeometryBlock(PhysicalLayer *layer, byte **blockPtr)
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

  *blockPtr = (byte *) block;
  return VDO_SUCCESS;
}

/**********************************************************************/
int loadVolumeGeometry(PhysicalLayer *layer, VolumeGeometry *geometry)
{
  byte *block;
  int result = readGeometryBlock(layer, &block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  Buffer *buffer;
  result = wrapBuffer(block, VDO_BLOCK_SIZE, VDO_BLOCK_SIZE, &buffer);
  if (result != VDO_SUCCESS) {
    FREE(block);
    return result;
  }

  result = decodeGeometryBlock(buffer, geometry);
  if (result != VDO_SUCCESS) {
    freeBuffer(&buffer);
    FREE(block);
    return result;
  }

  // Checksum everything decoded so far.
  CRC32Checksum checksum = layer->updateCRC32(INITIAL_CHECKSUM, block,
                                              uncompactedAmount(buffer));
  CRC32Checksum savedChecksum;
  result = getUInt32LEFromBuffer(buffer, &savedChecksum);
  if (result != VDO_SUCCESS) {
    freeBuffer(&buffer);
    FREE(block);
    return result;
  }

  // Finished all decoding. Everything that follows is validation code.
  freeBuffer(&buffer);
  FREE(block);

  if (!isLoadableReleaseVersion(geometry->releaseVersion)) {
    return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                                   "release version %d cannot be loaded",
                                   geometry->releaseVersion);
  }

  return ((checksum == savedChecksum) ? VDO_SUCCESS : VDO_CHECKSUM_MISMATCH);
}

/************************************************************************/
int computeIndexBlocks(IndexConfig *indexConfig, BlockCount *indexBlocksPtr)
{
  UdsConfiguration udsConfiguration = NULL;
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

  *geometry = (VolumeGeometry) {
    .releaseVersion = CURRENT_RELEASE_VERSION_NUMBER,
    .nonce = nonce,
    .regions = {
      [INDEX_REGION] = {
        .id = INDEX_REGION,
        .startBlock = 1,
      },
      [DATA_REGION] = {
        .id = DATA_REGION,
        .startBlock = 1 + indexSize,
      }
    }
  };
  memcpy(geometry->uuid, uuid, sizeof(UUID));
  if (indexSize > 0) {
    memcpy(&geometry->indexConfig, indexConfig, sizeof(IndexConfig));
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

  Buffer *buffer;
  result = wrapBuffer((byte *) block, VDO_BLOCK_SIZE, 0, &buffer);
  if (result != VDO_SUCCESS) {
    FREE(block);
    return result;
  }

  result = encodeGeometryBlock(geometry, buffer);
  if (result != VDO_SUCCESS) {
    freeBuffer(&buffer);
    FREE(block);
    return result;
  }

  // Checksum everything encoded so far and then encode the checksum.
  CRC32Checksum checksum = layer->updateCRC32(INITIAL_CHECKSUM, (byte *) block,
                                              contentLength(buffer));
  result = putUInt32LEIntoBuffer(buffer, checksum);
  if (result != VDO_SUCCESS) {
    freeBuffer(&buffer);
    FREE(block);
    return result;
  }

  // Write it.
  result = layer->writer(layer, GEOMETRY_BLOCK_LOCATION, 1, block, NULL);
  freeBuffer(&buffer);
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
