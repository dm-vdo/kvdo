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
 * $Id: //eng/uds-releases/gloria/src/uds/threadSemaphore.h#1 $
 */

#ifndef THREAD_SEMAPHORE_H
#define THREAD_SEMAPHORE_H

#include "threadDefs.h"
#include "timeUtils.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize a semaphore used among threads in the same process.
 *
 * @param semaphore the semaphore to initialize
 * @param value     the initial value of the semaphore
 * @param context   the calling context (for logging)
 *
 * @return UDS_SUCCESS or an error code
 **/
int initializeSemaphore(Semaphore    *semaphore,
                        unsigned int  value,
                        const char   *context)
  __attribute__((warn_unused_result));

/**
 * Destroy a semaphore used among threads in the same process.
 *
 * @param semaphore the semaphore to destroy
 * @param context   the calling context (for logging)
 *
 * @return UDS_SUCCESS or an error code
 **/
int destroySemaphore(Semaphore *semaphore, const char *context);

/**
 * Acquire a permit from a semaphore, waiting if none are currently available.
 *
 * @param semaphore the semaphore to acquire
 * @param context   the calling context (for logging)
 **/
void acquireSemaphore(Semaphore *semaphore, const char *context);

/**
 * Attempt to acquire a permit from a semaphore.
 *
 * If a permit is available, it is claimed and the function immediately
 * returns true. If a timeout is zero or negative, the function immediately
 * returns false. Otherwise, this will wait either a permit to become
 * available (returning true) or the relative timeout to expire (returning
 * false).
 *
 * @param semaphore the semaphore to decrement
 * @param timeout   the relative time until the timeout expires
 * @param context   the calling context (for logging)
 *
 * @return true if a permit was acquired, otherwise false
 **/
bool attemptSemaphore(Semaphore  *semaphore,
                      RelTime     timeout,
                      const char *context)
  __attribute__((warn_unused_result));

/**
 * Release a semaphore, incrementing the number of available permits.
 *
 * @param semaphore the semaphore to increment
 * @param context   the calling context (for logging)
 **/
void releaseSemaphore(Semaphore *semaphore, const char *context);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* THREAD_SEMAPHORE_H */
