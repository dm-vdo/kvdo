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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/workQueueHandle.c#2 $
 */

#include "workQueueHandle.h"

WorkQueueStackHandleGlobals workQueueStackHandleGlobals;

/**********************************************************************/
void initializeWorkQueueStackHandle(WorkQueueStackHandle *handle,
                                    SimpleWorkQueue      *queue)
{
  handle->nonce = workQueueStackHandleGlobals.nonce;
  handle->queue = queue;

  long offset = (char *) handle - getBottomOfStack();
  spin_lock(&workQueueStackHandleGlobals.offsetLock);
  if (workQueueStackHandleGlobals.offset == 0) {
    workQueueStackHandleGlobals.offset = offset;
    spin_unlock(&workQueueStackHandleGlobals.offsetLock);
  } else {
    long foundOffset = workQueueStackHandleGlobals.offset;
    spin_unlock(&workQueueStackHandleGlobals.offsetLock);
    BUG_ON(foundOffset != offset);
  }
}

/**********************************************************************/
void initWorkQueueStackHandleOnce(void)
{
  spin_lock_init(&workQueueStackHandleGlobals.offsetLock);
  workQueueStackHandleGlobals.nonce = currentTime(CT_MONOTONIC);
}
