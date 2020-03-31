/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoLoad.h#6 $
 */

#ifndef VDO_LOAD_H
#define VDO_LOAD_H

#include "volumeGeometry.h"
#include "types.h"

/**
 * A function which decodes a vdo from a super block.
 *
 * @param vdo              The vdo to be decoded (its super block must already
 *                         be loaded)
 * @param validate_config  If <code>true</code>, the vdo's configuration will
 *                         be validated before the decode is attempted
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int VDODecoder(struct vdo *vdo, bool validate_config);

/**
 * Load a vdo for normal operation. This method must not be called from a base
 * thread.
 *
 * @param vdo         The vdo to load
 *
 * @return VDO_SUCCESS or an error
 **/
int perform_vdo_load(struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Perpare a vdo for loading by reading structures off disk. This method does
 * not alter the on-disk state. It should be called from the vdo constructor,
 * whereas perform_vdo_load() will be called during pre-resume if the vdo has
 * not been resumed before.
 **/
int prepare_to_load_vdo(struct vdo *vdo,
			const struct vdo_load_config *load_config)
	__attribute__((warn_unused_result));

/**
 * Synchronously load a vdo from a specified super block location for use by
 * user-space tools.
 *
 * @param [in]  layer            The physical layer the vdo sits on
 * @param [in]  geometry         A pointer to the geometry for the volume
 * @param [in]  validate_config  Whether to validate the vdo against the layer
 * @param [in]  decoder          The vdo decoder to use, if NULL, the default
 *                               decoder will be used
 * @param [out] vdo_ptr          A pointer to hold the decoded vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int load_vdo_superblock(PhysicalLayer *layer,
			struct volume_geometry *geometry,
			bool validate_config,
			VDODecoder *decoder,
			struct vdo **vdo_ptr)
	__attribute__((warn_unused_result));

/**
 * Synchronously load a vdo volume for use by user-space tools.
 *
 * @param [in]  layer            The physical layer the vdo sits on
 * @param [in]  validate_config  Whether to validate the vdo against the layer
 * @param [in]  decoder          The vdo decoder to use, if NULL, the default
 *                               decoder will be used
 * @param [out] vdo_ptr          A pointer to hold the decoded vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int load_vdo(PhysicalLayer *layer,
	     bool validate_config,
	     VDODecoder *decoder,
	     struct vdo **vdo_ptr)
	__attribute__((warn_unused_result));

#endif /* VDO_LOAD_H */
