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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoLoad.h#8 $
 */

#ifndef VDO_LOAD_H
#define VDO_LOAD_H

#include "types.h"

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

#endif /* VDO_LOAD_H */
