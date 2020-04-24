/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/krusty/src/uds/threads.h#3 $
 */

#ifndef THREADS_H
#define THREADS_H

#include "compiler.h"
#include "threadOnce.h"
#include "timeUtils.h"
#include "uds-error.h"

#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include "util/eventCount.h"

typedef struct { struct event_count *eventCount; } CondVar;
typedef struct mutex                       Mutex;
typedef struct semaphore                   Semaphore;
typedef struct kernelThread               *Thread;

typedef struct {
  Semaphore mutex;       // Mutex for this barrier object
  Semaphore wait;        // Semaphore for threads waiting at the barrier
  int       arrived;     // Number of threads which have arrived
  int       threadCount; // Total number of threads using this barrier
} Barrier;

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
 * Create a thread, logging any cause of failure.
 *
 * @param threadFunc  function to run in new thread
 * @param threadData  private data for new thread
 * @param name        name of the new thread
 * @param newThread   where to store the new thread id
 *
 * @return       success or failure indication
 **/
int createThread(void       (*threadFunc)(void *),
                 void        *threadData,
                 const char  *name,
                 Thread      *newThread)
  __attribute__((warn_unused_result));

/**
 * Retrieve the current numbers of cores.
 *
 * This is either the total number or the number of cores that this
 * process has been limited to.
 *
 * @return      number of cores
 **/
unsigned int getNumCores(void);

/**
 * Return the id of the current thread.
 *
 * @return the thread id
 **/
pid_t get_thread_id(void) __attribute__((warn_unused_result));


/**
 * Wait for termination of another thread.
 *
 *
 * @param th             The thread for which to wait.
 *
 * @return               UDS_SUCCESS or error code
 **/
int joinThreads(Thread th);

/**
 * Exit the current thread.  This is a kernel-only function that is intended to
 * be an alternative to using BUG() or BUG_ON().
 **/
__attribute__((noreturn))
void exitThread(void);

/**
 * Initialize a thread synchronization barrier (also known as a rendezvous).
 *
 * @param barrier      the barrier to initialize
 * @param threadCount  the number of threads that must enter the barrier before
 *                     any threads are permitted to leave it
 *
 * @return UDS_SUCCESS or an error code
 **/
int initializeBarrier(Barrier *barrier, unsigned int threadCount)
  __attribute__((warn_unused_result));

/**
 * Destroy a thread synchronization barrier.
 *
 * @param barrier   the barrier to destroy
 *
 * @return UDS_SUCCESS or an error code
 **/
int destroyBarrier(Barrier *barrier);

/**
 * Enter a thread synchronization barrier, waiting for the configured number
 * of threads to have entered before exiting the barrier. Exactly one thread
 * will be arbitrarily selected to be flagged as the "winner" of a barrier.
 *
 * @param barrier   the barrier to enter
 * @param winner    if non-NULL, a pointer to the flag indicating whether the
 *                  calling thread was the unique winner
 *
 * @return UDS_SUCCESS or an error code
 **/
int enterBarrier(Barrier *barrier, bool *winner);

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
 * Wait on a condition variable with a timeout.
 *
 * @param cond     condition variable to wait on
 * @param mutex    mutex to release while waiting
 * @param timeout  the relative time until the timeout expires
 *
 * @return error code (ETIMEDOUT if the deadline is hit)
 **/
int timedWaitCond(CondVar *cond, Mutex *mutex, RelTime timeout);

/**
 * Destroy a condition variable.
 *
 * @param cond  condition variable to destroy
 *
 * @return      UDS_SUCCESS or error code
 **/
int destroyCond(CondVar *cond);


/**
 * Initialize the default type (error-checking during development) mutex.
 *
 * @param mutex the mutex to initialize
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
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
 * Initialize a semaphore used among threads in the same process.
 *
 * @param semaphore the semaphore to initialize
 * @param value     the initial value of the semaphore
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static INLINE int initializeSemaphore(Semaphore *semaphore, unsigned int value)
{
  sema_init(semaphore, value);
  return UDS_SUCCESS;
}

/**
 * Destroy a semaphore used among threads in the same process.
 *
 * @param semaphore the semaphore to destroy
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int destroySemaphore(Semaphore *semaphore)
{
  return UDS_SUCCESS;
}

/**
 * Acquire a permit from a semaphore, waiting if none are currently available.
 *
 * @param semaphore the semaphore to acquire
 **/
static INLINE void acquireSemaphore(Semaphore *semaphore)
{
  // Do not use down(semaphore).  Instead use down_interruptible so that we do
  // not get 120 second stall messages in kern.log.
  while (down_interruptible(semaphore) != 0) {
  }
}

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
 *
 * @return true if a permit was acquired, otherwise false
 **/
__attribute__((warn_unused_result))
static INLINE bool attemptSemaphore(Semaphore *semaphore, RelTime timeout)
{
  if (timeout <= 0) {
    // No timeout, just try to grab the semaphore.
    return down_trylock(semaphore) == 0;
  } else {
    unsigned int jiffies = usecs_to_jiffies(relTimeToMicroseconds(timeout));
    return down_timeout(semaphore, jiffies) == 0;
  }
}

/**
 * Release a semaphore, incrementing the number of available permits.
 *
 * @param semaphore the semaphore to increment
 **/
static INLINE void releaseSemaphore(Semaphore *semaphore)
{
  up(semaphore);
}

/**
 * Yield the time slice in the given thread.
 *
 * @return UDS_SUCCESS or an error code
 **/
int yieldScheduler(void);


#endif /* THREADS_H */
