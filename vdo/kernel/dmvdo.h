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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/dmvdo.h#2 $
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
} KVDOStatus;

/*
 * The internal representation of our device.
 */
struct kvdoDevice {
  KVDOStatus          status;
  struct kobject      kobj;
};

extern struct kvdoDevice kvdoDevice;

#endif /* DMVDO_H */
