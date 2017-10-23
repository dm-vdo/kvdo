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
 * $Id: //eng/uds-releases/flanders/src/uds/threadMutex.h#2 $
 */

#ifndef THREAD_MUTEX_H
#define THREAD_MUTEX_H

#include "threadDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize a mutex, optionally asserting if the mutex initialization
 * fails. This function should only be called directly in places where making
 * assertions is not safe.
 *
 * @param mutex         the mutex to initialize
 * @param assertOnError if <code>true</code>, an error initializing the
 *                      mutex will make an assertion
 *
 * @return UDS_SUCCESS or an error code
 **/
int initializeMutex(Mutex *mutex, bool assertOnError);

/**
 * Initialize the default type (error-checking during development)
 * mutex.
 *
 * @param mutex the mutex to initialize
 *
 * @return      UDS_SUCCESS or an error code
 **/
int initMutex(Mutex *mutex);

/**
 * Destroy a mutex (with error checking during development).
 *
 * @param mutex mutex to destroy
 *
 * @return      UDS_SUCCESS or error code
 **/
int destroyMutex(Mutex *mutex);

/**
 * Lock a mutex, with optional error checking during development.
 *
 * @param mutex mutex to lock
 **/
void lockMutex(Mutex *mutex);

/**
 * Unlock a mutex, with optional error checking during development.
 *
 * @param mutex mutex to unlock
 **/
void unlockMutex(Mutex *mutex);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* THREAD_MUTEX_H */
