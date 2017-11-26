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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/volumeGeometry.h#2 $
 */

#ifndef VOLUME_GEOMETRY_H
#define VOLUME_GEOMETRY_H

#include "uds.h"

#include "types.h"

struct indexConfig {
  uint32_t mem;
  uint32_t checkpointFrequency;
  bool     sparse;
  } __attribute__((packed));

typedef enum {
  INDEX_REGION = 0,
  DATA_REGION  = 1,
  VOLUME_REGION_COUNT,
} VolumePartitionID;

typedef struct {
  /** The ID of the partition */
  VolumePartitionID   id;
  /**
   * The absolute starting offset on the device. The partition continues until
   * the next partition begins.
   */
  PhysicalBlockNumber startBlock;
} __attribute__((packed)) VolumePartition;

typedef struct {
  /** The nonce of this volume */
  Nonce           nonce;
  /** The partitions in ID order */
  VolumePartition partitions[VOLUME_REGION_COUNT];
  /** The index config */
  IndexConfig     indexConfig;
} __attribute__((packed)) VolumeGeometry;

/**
 * Read a geometry block.
 *
 * @param [in]  layer        The layer to read and parse the geometry from
 * @param [out] geometryPtr  A pointer to return the geometry in
 **/
int loadVolumeGeometry(PhysicalLayer *layer, VolumeGeometry **geometryPtr)
__attribute__((warn_unused_result));

/**
 * Write a geometry block for a VDO.
 *
 * @param layer        The layer for the VDO
 * @param nonce        The nonce for the VDO
 * @param indexConfig  The index config of the VDO.
 *
 * @return VDO_SUCCESS or an error
 **/
int writeVolumeGeometry(PhysicalLayer *layer,
                        Nonce          nonce,
                        IndexConfig   *indexConfig)
__attribute__((warn_unused_result));

/**
 * Convert a index config to a UDS configuration, which can be used by UDS.
 *
 * @param [in]  indexConfig   The index config to convert
 * @param [out] udsConfigPtr  A pointer to return the UDS configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int indexConfigToUdsConfiguration(IndexConfig      *indexConfig,
                                  UdsConfiguration *udsConfigPtr)
__attribute__((warn_unused_result));

#endif // VOLUME_GEOMETRY_H
