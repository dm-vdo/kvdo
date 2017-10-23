/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/kernelLinux/uds/threadMutexLinuxKernel.c#2 $
 */

#include <linux/kthread.h>

#include "errors.h"
#include "memoryAlloc.h"
#include "threadMutex.h"

/**********************************************************************/
int initializeMutex(Mutex *mutex, bool assertOnError)
{
  int result = ALLOCATE(1, struct mutex, __func__, &mutex->pmut);
  if (result == UDS_SUCCESS) {
    mutex_init(mutex->pmut);
  } else if (assertOnError) {
    result = ASSERT_WITH_ERROR_CODE((result == UDS_SUCCESS), result,
                                    "mutex: 0x%p", (void *) mutex);
  }
  return result;
}

/**********************************************************************/
int initMutex(Mutex *mutex)
{
  return initializeMutex(mutex, false);
}

/**********************************************************************/
int destroyMutex(Mutex *mutex)
{
  FREE(mutex->pmut);
  mutex->pmut = NULL;
  return UDS_SUCCESS;
}

/**********************************************************************/
void lockMutex(Mutex *mutex)
{
  mutex_lock(mutex->pmut);
}

/**********************************************************************/
void unlockMutex(Mutex *mutex)
{
  mutex_unlock(mutex->pmut);
}
