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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dmvdo.h#4 $
 */

#ifndef DMVDO_H
#define DMVDO_H

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/types.h>

#include "kernelLayer.h"

enum vdo_module_status {
	VDO_MODULE_UNINITIALIZED = 0,
	VDO_MODULE_READY,
	VDO_MODULE_SHUTTING_DOWN,
};

/*
 * The global storage structure for the vdo kernel module.
 */
struct vdo_module_globals {
	enum vdo_module_status status;
	struct kobject kobj;
};

#endif /* DMVDO_H */
