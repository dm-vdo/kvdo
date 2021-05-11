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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/sysfs.h#2 $
 */

#ifndef ALBIREO_SYSFS_H
#define ALBIREO_SYSFS_H

#include "kernelLayer.h"

struct kvdoDevice;

/**
* Initializes the sysfs objects global to all vdo devices.
*
* @param deviceObject  the kobject of the kvdoDevice to initialize.
*/
int vdoInitSysfs(struct kobject *deviceObject);

/**
 * Releases the global sysfs objects.
 *
 * @param deviceObject  the kobject of the kvdoDevice to release.
 */
void vdoPutSysfs(struct kobject *deviceObject);

#endif /* ALBIREO_SYSFS_H */
