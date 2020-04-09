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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/kernelVDOInternals.h#3 $
 */

#ifndef KERNEL_VDO_INTERNALS_H
#define KERNEL_VDO_INTERNALS_H

#include "kernelVDO.h"

/**
 * Invoke the callback in a completion from a base code thread.
 *
 * @param completion   The completion to run
 **/
void kvdoInvokeCallback(VDOCompletion *completion);

/**
 * Enqueue a work item to be performed in the base code in a
 * particular thread.
 *
 * @param thread         The KVDO thread on which to run the work item
 * @param item           The work item to be run
 **/
void enqueueKVDOThreadWork(KVDOThread *thread, KvdoWorkItem *item);

/**********************************************************************/
typedef struct {
  KvdoWorkItem       workItem;
  KVDO              *kvdo;
  void              *data;
  struct completion *completion;
} SyncQueueWork;

/**
 * Initiate an arbitrary asynchronous base-code operation and wait for
 * it.
 *
 * An async queue operation is performed and we wait for completion.
 *
 * @param kvdo       The kvdo data handle
 * @param action     The operation to perform
 * @param data       Unique data that can be used by the operation
 * @param threadID   The thread on which to perform the operation
 * @param completion The completion to wait on
 *
 * @return VDO_SUCCESS of an error code
 **/
void performKVDOOperation(KVDO              *kvdo,
                          KvdoWorkFunction   action,
                          void              *data,
                          ThreadID           threadID,
                          struct completion *completion);

#endif // KERNEL_VDO_INTERNALS_H
