/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/threadDevice.h#1 $
 */

#include "kernelLayer.h"

/**
 * Temporarily register the current thread as being associated with a
 * VDO device id number, for logging purposes.
 *
 * Any such registered thread must later be unregistered via
 * unregisterThreadDeviceID.
 *
 * The pointed-to ID number should be nonzero.
 *
 * @param newThread  RegisteredThread structure to use for the current thread
 * @param idPtr      Location where the ID number is stored
 **/
void registerThreadDeviceID(RegisteredThread *newThread, unsigned int *idPtr);

/**
 * Temporarily register the current thread as being associated with an
 * existing VDO device, for logging purposes.
 *
 * Any such registered thread must later be unregistered via
 * unregisterThreadDeviceID.
 *
 * @param newThread  RegisteredThread structure to use for the current thread
 * @param layer      The KernelLayer object for the VDO device
 **/
static inline void registerThreadDevice(RegisteredThread *newThread,
                                        KernelLayer      *layer)
{
  registerThreadDeviceID(newThread, &layer->instance);
}

/**
 * Cancel registration of the current thread as being associated with
 * a VDO device or device ID number.
 **/
void unregisterThreadDeviceID(void);

/**
 * Get the VDO device ID number temporarily associated with the
 * current thread, if any.
 *
 * @return  the device ID number, if any, or -1
 **/
int getThreadDeviceID(void);

/**
 * Initialize the thread device-ID registry.
 **/
void initializeThreadDeviceRegistry(void);
