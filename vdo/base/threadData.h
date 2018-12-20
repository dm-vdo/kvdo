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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/threadData.h#5 $
 */

#ifndef THREAD_DATA_H
#define THREAD_DATA_H

#include "completion.h"

/**
 * Create and initialize an array of ThreadData structures for all the base
 * threads in the VDO.
 *
 * @param [in]  isReadOnly    <code>true</code> if the threads should be in
 *                            read-only mode
 * @param [in]  threadConfig  The thread configuration of the VDO
 * @param [in]  layer         The physical layer of the VDO
 * @param [out] threadsPtr    A pointer to receive the new array
 *
 * @return VDO_SUCCESS or an error
 **/
int makeThreadDataArray(bool                 isReadOnly,
                        const ThreadConfig  *threadConfig,
                        PhysicalLayer       *layer,
                        ThreadData         **threadsPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy and free an array of ThreadData structures, then null out the
 * reference to it.
 *
 * @param threadsPtr  The reference to the array to free
 * @param count       The number of thread structures in the array
 **/
void freeThreadDataArray(ThreadData **threadsPtr, ThreadCount count);

/**
 * Wait until no threads are entering read-only mode.
 *
 * @param vdo     The VDO to wait on
 * @param waiter  The completion to notify when no threads are entering
 *                read-only mode
 **/
void waitUntilNotEnteringReadOnlyMode(VDO *vdo, VDOCompletion *waiter);

/**
 * Put a VDO into read-only mode and save the read-only state in the super
 * block.
 *
 * @param vdo             The VDO to put into read-only mode
 * @param errorCode       The error which caused the VDO to enter read-only
 *                        mode
 **/
void makeVDOReadOnly(VDO *vdo, int errorCode);

#endif /* THREAD_DATA_H */
