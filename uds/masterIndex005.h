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
 * $Id: //eng/uds-releases/gloria/src/uds/masterIndex005.h#1 $
 */

#ifndef MASTERINDEX005_H
#define MASTERINDEX005_H 1

#include "masterIndexOps.h"

/**
 * Make a new master index.
 *
 * @param config          The configuration of the master index
 * @param numZones        The number of zones
 * @param volumeNonce     The nonce used to authenticate the index
 * @param masterIndex     Location to hold new master index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
int makeMasterIndex005(const Configuration *config, unsigned int numZones,
                       uint64_t volumeNonce, MasterIndex **masterIndex)
  __attribute__((warn_unused_result));

/**
 * Compute the number of bytes required to save a master index of a given
 * configuration.
 *
 * @param config    The configuration of the master index
 * @param numBytes  The number of bytes required to save the master index
 *
 * @return UDS_SUCCESS or an error code.
 **/
int computeMasterIndexSaveBytes005(const Configuration *config,
                                   size_t *numBytes)
  __attribute__((warn_unused_result));

#endif /* MASTERINDEX005_H */
