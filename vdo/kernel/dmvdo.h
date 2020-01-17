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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dmvdo.h#3 $
 */

#ifndef DMVDO_H
#define DMVDO_H

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/types.h>

#include "kernelLayer.h"

typedef enum {
	UNINITIALIZED = 0,
	READY,
	SHUTTING_DOWN,
} kvdo_status;

/*
 * The global storage structure for the KVDO module.
 */
struct kvdo_module_globals {
	kvdo_status status;
	struct kobject kobj;
};

#endif /* DMVDO_H */
