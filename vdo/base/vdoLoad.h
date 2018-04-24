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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoLoad.h#2 $
 */

#ifndef VDO_LOAD_H
#define VDO_LOAD_H

#include "volumeGeometry.h"
#include "types.h"

/**
 * A function which decodes a VDO from a super block.
 *
 * @param vdo             The VDO to be decoded (its super block must already
 *                        be loaded)
 * @param validateConfig  If <code>true</code>, the VDO's configuration will
 *                        be validated before the decode is attempted
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int VDODecoder(VDO *vdo, bool validateConfig);

/**
 * Load a VDO for normal operation. This method must not be called from a base
 * thread.
 *
 * @param vdo         The VDO to load
 * @param loadConfig  A pointer to the load-time configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int performVDOLoad(VDO *vdo, const VDOLoadConfig *loadConfig)
  __attribute__((warn_unused_result));

/**
 * Synchronously load a VDO from a specified super block location for use by
 * user-space tools.
 *
 * @param [in]  layer           The physical layer the VDO sits on
 * @param [in]  geometry        A pointer to the geometry for the volume
 * @param [in]  validateConfig  Whether to validate the VDO against the layer
 * @param [in]  decoder         The VDO decoder to use, if NULL, the default
 *                              decoder will be used
 * @param [out] vdoPtr          A pointer to hold the decoded VDO
 *
 * @return VDO_SUCCESS or an error
 **/
int loadVDOSuperblock(PhysicalLayer   *layer,
                      VolumeGeometry  *geometry,
                      bool             validateConfig,
                      VDODecoder      *decoder,
                      VDO            **vdoPtr)
  __attribute__((warn_unused_result));

/**
 * Synchronously load a VDO volume for use by user-space tools.
 *
 * @param [in]  layer           The physical layer the VDO sits on
 * @param [in]  validateConfig  Whether to validate the VDO against the layer
 * @param [in]  decoder         The VDO decoder to use, if NULL, the default
 *                              decoder will be used
 * @param [out] vdoPtr          A pointer to hold the decoded VDO
 *
 * @return VDO_SUCCESS or an error
 **/
int loadVDO(PhysicalLayer  *layer,
            bool            validateConfig,
            VDODecoder     *decoder,
            VDO           **vdoPtr)
  __attribute__((warn_unused_result));

#endif /* VDO_LOAD_H */
