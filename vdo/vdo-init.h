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
 */

#ifndef VDO_INIT_H
#define VDO_INIT_H

#include <linux/device-mapper.h>
#include <linux/kobject.h>

#include "device-config.h"
#include "types.h"


const char * __must_check
vdo_get_device_name(const struct dm_target *target);

int __must_check
vdo_initialize_internal(struct vdo *vdo,
			struct device_config *config,
			unsigned int instance,
			char **reason);

#endif /* VDO_INIT_H */
