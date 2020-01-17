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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/threadDevice.c#2 $
 */

#include "threadDevice.h"

#include "threadRegistry.h"

/*
 * A registry of all threads temporarily associated with particular
 * VDO devices.
 */
static ThreadRegistry device_id_thread_registry;

/**********************************************************************/
void register_thread_device_id(RegisteredThread *new_thread,
			       unsigned int *id_ptr)
{
	registerThread(&device_id_thread_registry, new_thread, id_ptr);
}

/**********************************************************************/
void unregister_thread_device_id(void)
{
	unregisterThread(&device_id_thread_registry);
}

/**********************************************************************/
int get_thread_device_id(void)
{
	const unsigned int *pointer = lookupThread(&device_id_thread_registry);
	return pointer ? *pointer : -1;
}

/**********************************************************************/
void initialize_thread_device_registry(void)
{
	initializeThreadRegistry(&device_id_thread_registry);
}
