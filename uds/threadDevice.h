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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/threadDevice.h#1 $
 */

#ifndef UDS_THREAD_DEVICE_H
#define UDS_THREAD_DEVICE_H

#include "threadRegistry.h"

/**
 * Temporarily register the current thread as being associated with a
 * VDO device id number, for logging purposes.
 *
 * Any such registered thread must later be unregistered via
 * unregister_thread_device_id.
 *
 * The pointed-to ID number should be nonzero.
 *
 * @param new_thread  registered_thread structure to use for the current thread
 * @param id_ptr      Location where the ID number is stored
 **/
void uds_register_thread_device_id(struct registered_thread *new_thread,
				   unsigned int *id_ptr);

/**
 * Cancel registration of the current thread as being associated with
 * a VDO device or device ID number.
 **/
void uds_unregister_thread_device_id(void);

/**
 * Get the VDO device ID number temporarily associated with the
 * current thread, if any.
 *
 * @return the device ID number, if any, or -1
 **/
int uds_get_thread_device_id(void);

/**
 * Initialize the thread device-ID registry.
 **/
void uds_initialize_thread_device_registry(void);

#endif /* UDS_THREAD_DEVICE_H */
