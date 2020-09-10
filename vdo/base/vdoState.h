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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoState.h#6 $
 */

#ifndef VDO_STATE_H
#define VDO_STATE_H

#include "compiler.h"

/**
 * The current operating mode of the VDO. These are persistent on disk
 * so the values must not change.
 **/
typedef enum {
	VDO_DIRTY = 0,
	VDO_NEW = 1,
	VDO_CLEAN = 2,
	VDO_READ_ONLY_MODE = 3,
	VDO_FORCE_REBUILD = 4,
	VDO_RECOVERING = 5,
	VDO_REPLAYING = 6,
	VDO_REBUILD_FOR_UPGRADE = 7,

	// Keep VDO_STATE_COUNT at the bottom.
	VDO_STATE_COUNT
} VDOState;

/**
 * Get the name of a VDO state code for logging purposes.
 *
 * @param state  The state code
 *
 * @return The name of the state code
 **/
const char * __must_check get_vdo_state_name(VDOState state);

/**
 * Return a user-visible string describing the current VDO state.
 *
 * @param state  The VDO state to describe
 *
 * @return A string constant describing the state
 **/
const char * __must_check describe_vdo_state(VDOState state);

#endif // VDO_STATE_H
