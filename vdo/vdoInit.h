/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/vdoInit.h#2 $
 */

#ifndef VDO_INIT_H
#define VDO_INIT_H

#include <linux/device-mapper.h>
#include <linux/kobject.h>

#include "deviceConfig.h"
#include "types.h"

/**
 * Get the device name associated with the vdo target
 *
 * @param target  The target device interface
 *
 * @return The block device name
 **/
const char * __must_check
get_vdo_device_name(const struct dm_target *target);

/**
 * Perform the first steps in initializing a vdo as part of device creation.
 *
 * @param vdo     The vdo being initialized
 * @param layer   The physical layer on which the VDO resides
 * @param config  The configuration of the vdo being initialized
 * @param parent  The parent of the vdo's sysfs nodes
 * @param reason  A pointer to hold an error message on failure
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
initialize_vdo(struct vdo *vdo,
	       PhysicalLayer *layer,
	       struct device_config *config,
	       struct kobject *parent,
	       char **reason);

#endif // VDO_INIT_H
