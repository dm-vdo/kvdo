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
 * $Id: //eng/uds-releases/gloria/src/uds/threadCondVar.h#2 $
 */

#ifndef THREAD_COND_VAR_H
#define THREAD_COND_VAR_H

#include "threadDefs.h"
#include "timeUtils.h"

/**
 * Initialize a condition variable with default attributes.
 *
 * @param cond       condition variable to init
 *
 * @return           UDS_SUCCESS or error code
 **/
int initCond(CondVar *cond) __attribute__((warn_unused_result));

/**
 * Signal a condition variable.
 *
 * @param cond  condition variable to signal
 *
 * @return      UDS_SUCCESS or error code
 **/
int signalCond(CondVar *cond);

/**
 * Broadcast a condition variable.
 *
 * @param cond  condition variable to broadcast
 *
 * @return      UDS_SUCCESS or error code
 **/
int broadcastCond(CondVar *cond);

/**
 * Wait on a condition variable.
 *
 * @param cond    condition variable to wait on
 * @param mutex   mutex to release while waiting
 *
 * @return        UDS_SUCCESS or error code
 **/
int waitCond(CondVar *cond, Mutex *mutex);

/**
 * Wait on a condition variable or until a specified time.
 *
 * @param cond      condition variable to wait on
 * @param mutex     mutex to release while waiting
 * @param deadline  absolute timeout
 *
 * @return error code (ETIMEDOUT if the deadline is hit)
 **/
int timedWaitCond(CondVar *cond, Mutex *mutex, const AbsTime *deadline);

/**
 * Destroy a condition variable.
 *
 * @param cond  condition variable to destroy
 *
 * @return      UDS_SUCCESS or error code
 **/
int destroyCond(CondVar *cond);

#endif /* THREAD_COND_VAR_H */
