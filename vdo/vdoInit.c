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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/vdoInit.c#1 $
 */

#include <linux/device-mapper.h>
#include <linux/kobject.h>

#include "adminCompletion.h"
#include "poolSysfs.h"
#include "types.h"
#include "vdoInternal.h"

/**********************************************************************/
const char *get_vdo_device_name(const struct dm_target *target)
{
	return dm_device_name(dm_table_get_md(target->table));
}

/**
 * Initialize the vdo and work queue sysfs directories.
 *
 * @param vdo     The vdo being initialized
 * @param target  The device-mapper target this vdo is
 * @param parent  The parent directory object
 * @param reason  A pointer to hold an error message on failure
 *
 * @return VDO_SUCCESS or an error code
 **/
static int initialize_vdo_kobjects(struct vdo *vdo,
				   struct dm_target *target,
				   struct kobject *parent,
				   char **reason)
{
	int result;
	kobject_init(&vdo->vdo_directory, &vdo_directory_type);
	result = kobject_add(&vdo->vdo_directory,
			     parent,
			     get_vdo_device_name(target));
	if (result != 0) {
		*reason = "Cannot add sysfs node";
		kobject_put(&vdo->vdo_directory);
		return result;
	}

	kobject_init(&vdo->work_queue_directory,
		     &work_queue_directory_type);
	result = kobject_add(&vdo->work_queue_directory,
			     &vdo->vdo_directory,
			     "work_queues");
	if (result != 0) {
		*reason = "Cannot add sysfs node";
		kobject_put(&vdo->work_queue_directory);
		kobject_put(&vdo->vdo_directory);
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int initialize_vdo(struct vdo *vdo,
		   PhysicalLayer *layer,
		   struct dm_target *target,
		   struct kobject *parent,
		   const char *device_name,
		   char **reason)
{
	vdo->layer = layer;
	initialize_admin_completion(vdo, &vdo->admin_completion);

	/*
	 * After this point, calling kobject_put on vdo_directory will
	 * decrement its reference count, and when the count goes to 0 the
	 * struct kernel_layer (which contains this vdo) will be freed.
	 */
	return initialize_vdo_kobjects(vdo, target, parent, reason);
}
