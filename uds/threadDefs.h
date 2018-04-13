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
 * $Id: //eng/uds-releases/gloria/kernelLinux/uds/threadDefs.h#1 $
 */

#ifndef LINUX_KERNEL_THREAD_DEFS_H
#define LINUX_KERNEL_THREAD_DEFS_H

#include <linux/mutex.h>
#include <linux/semaphore.h>

#include "compiler.h"
#include "uds-error.h"
#include "util/eventCount.h"

typedef struct kernelThread *Thread;
typedef pid_t                ThreadId;

typedef struct { EventCount *eventCount;    } CondVar;
typedef struct mutex                           Mutex;
typedef struct { struct hr_semaphore *psem; } Semaphore;

typedef struct {
  Semaphore mutex;       // Mutex for this barrier object
  Semaphore wait;        // Semaphore for threads waiting at the barrier
  int       arrived;     // Number of threads which have arrived
  int       threadCount; // Total number of threads using this barrier
} Barrier;

/**
 * Initialize a mutex, optionally asserting if the mutex initialization fails.
 * In user mode, this function should only be called directly in places where
 * making assertions is not safe.  In kernel mode, it does not matter.
 *
 * @param mutex         the mutex to initialize
 * @param assertOnError if <code>true</code>, an error initializing the
 *                      mutex will make an assertion
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int initializeMutex(Mutex *mutex,
                                  bool   assertOnError __attribute__((unused)))
{
  mutex_init(mutex);
  return UDS_SUCCESS;
}

/**
 * Initialize the default type (error-checking during development) mutex.
 *
 * @param mutex the mutex to initialize
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int initMutex(Mutex *mutex)
{
  mutex_init(mutex);
  return UDS_SUCCESS;
}

/**
 * Destroy a mutex (with error checking during development).
 *
 * @param mutex mutex to destroy
 *
 * @return UDS_SUCCESS or error code
 **/
static INLINE int destroyMutex(Mutex *mutex)
{
  return UDS_SUCCESS;
}

/**
 * Lock a mutex, with optional error checking during development.
 *
 * @param mutex mutex to lock
 **/
static INLINE void lockMutex(Mutex *mutex)
{
  mutex_lock(mutex);
}

/**
 * Unlock a mutex, with optional error checking during development.
 *
 * @param mutex mutex to unlock
 **/
static INLINE void unlockMutex(Mutex *mutex)
{
  mutex_unlock(mutex);
}

/**
 * Apply a function to every thread that we have created.
 *
 * @param applyFunc  The function to apply
 * @param argument   The first argument to applyFunc
 *
 **/
void applyToThreads(void applyFunc(void *, struct task_struct *),
                    void *argument);

/**
 * Exit the current thread.  This is a platform-specific function that is
 * intended to be an alternative to using BUG() or BUG_ON().
 **/
__attribute__((noreturn))
void exitThread(void);

#endif // LINUX_KERNEL_THREAD_DEFS_H
