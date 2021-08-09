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
 * $Id: //eng/uds-releases/lisa/kernelLinux/uds/threadDevice.c#1 $
 */

#include "threadDevice.h"

#include "threadRegistry.h"

/*
 * A registry of all threads temporarily associated with particular
 * VDO devices.
 */
static struct thread_registry device_id_thread_registry;

/**********************************************************************/
void uds_register_thread_device_id(struct registered_thread *new_thread,
				   unsigned int *id_ptr)
{
	uds_register_thread(&device_id_thread_registry, new_thread, id_ptr);
}

/**********************************************************************/
void uds_unregister_thread_device_id(void)
{
	uds_unregister_thread(&device_id_thread_registry);
}

/**********************************************************************/
int uds_get_thread_device_id(void)
{
	const unsigned int *pointer =
		uds_lookup_thread(&device_id_thread_registry);

	return pointer ? *pointer : -1;
}

/**********************************************************************/
void uds_initialize_thread_device_registry(void)
{
	uds_initialize_thread_registry(&device_id_thread_registry);
}
