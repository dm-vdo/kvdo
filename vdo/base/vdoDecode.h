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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoDecode.h#2 $
 */

#ifndef VDO_DECODE_H
#define VDO_DECODE_H

#include "types.h"
#include "vdoComponentStates.h"

/**
 * Start the process of decoding the component data in a VDO super block.
 *
 * @param vdo              The VDO to decode
 * @param validate_config  If <code>true</code>, validate the VDO's config
 * @param states           A structure to hold the component states decoded
 *                         from the super block
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
start_vdo_decode(struct vdo *vdo,
		 bool validate_config,
		 struct vdo_component_states *states);

/**
 * Finish the process of decoding the component data in a VDO super block, now
 * that the layout has been decoded and validated.
 *
 * @param vdo     The VDO to decode
 * @param states  The component states decoded from the super block with which
 *                to recapitulate the vdo.
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check finish_vdo_decode(struct vdo *vdo,
				   struct vdo_component_states *states);

#endif /* VDO_DECODE_H */
