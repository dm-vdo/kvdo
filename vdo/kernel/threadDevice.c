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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/threadDevice.c#1 $
 */

#include "threadDevice.h"

#include "threadRegistry.h"

/*
 * A registry of all threads temporarily associated with particular
 * VDO devices.
 */
static ThreadRegistry deviceIDThreadRegistry;

/**********************************************************************/
void registerThreadDeviceID(RegisteredThread *newThread, unsigned int *idPtr)
{
  registerThread(&deviceIDThreadRegistry, newThread, idPtr);
}

/**********************************************************************/
void unregisterThreadDeviceID(void)
{
  unregisterThread(&deviceIDThreadRegistry);
}

/**********************************************************************/
int getThreadDeviceID(void)
{
  const unsigned int *pointer = lookupThread(&deviceIDThreadRegistry);
  return pointer ? *pointer : -1;
}

/**********************************************************************/
void initializeThreadDeviceRegistry(void)
{
  initializeThreadRegistry(&deviceIDThreadRegistry);
}
