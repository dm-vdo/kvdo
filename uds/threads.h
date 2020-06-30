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
 * $Id: //eng/uds-releases/krusty/src/uds/threads.h#8 $
 */

#ifndef THREADS_H
#define THREADS_H

#include "compiler.h"
#include "threadOnce.h"
#include "timeUtils.h"
#include "uds-error.h"

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include "util/eventCount.h"

struct cond_var { struct event_count *eventCount; };
struct thread;

struct barrier {
  struct semaphore mutex;       // Mutex for this barrier object
  struct semaphore wait;        // Semaphore for threads waiting at the barrier
  int              arrived;     // Number of threads which have arrived
  int              threadCount; // Total number of threads using this barrier
};

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
int __must_check createThread(void          (*threadFunc)(void *),
                              void           *threadData,
                              const char     *name,
                              struct thread **newThread);

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
pid_t __must_check get_thread_id(void);


/**
 * Wait for termination of another thread.
 *
 *
 * @param th             The thread for which to wait.
 *
 * @return               UDS_SUCCESS or error code
 **/
int joinThreads(struct thread *th);

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
int __must_check initializeBarrier(struct barrier *barrier,
                                   unsigned int threadCount);

/**
 * Destroy a thread synchronization barrier.
 *
 * @param barrier   the barrier to destroy
 *
 * @return UDS_SUCCESS or an error code
 **/
int destroyBarrier(struct barrier *barrier);

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
int enterBarrier(struct barrier *barrier, bool *winner);

/**
 * Initialize a condition variable with default attributes.
 *
 * @param cond       condition variable to init
 *
 * @return           UDS_SUCCESS or error code
 **/
int __must_check initCond(struct cond_var *cond);

/**
 * Signal a condition variable.
 *
 * @param cond  condition variable to signal
 *
 * @return      UDS_SUCCESS or error code
 **/
int signalCond(struct cond_var *cond);

/**
 * Broadcast a condition variable.
 *
 * @param cond  condition variable to broadcast
 *
 * @return      UDS_SUCCESS or error code
 **/
int broadcastCond(struct cond_var *cond);

/**
 * Wait on a condition variable.
 *
 * @param cond    condition variable to wait on
 * @param mutex   mutex to release while waiting
 *
 * @return        UDS_SUCCESS or error code
 **/
int waitCond(struct cond_var *cond, struct mutex *mutex);

/**
 * Wait on a condition variable with a timeout.
 *
 * @param cond     condition variable to wait on
 * @param mutex    mutex to release while waiting
 * @param timeout  the relative time until the timeout expires
 *
 * @return error code (ETIMEDOUT if the deadline is hit)
 **/
int timedWaitCond(struct cond_var *cond, struct mutex *mutex,
                  rel_time_t timeout);

/**
 * Destroy a condition variable.
 *
 * @param cond  condition variable to destroy
 *
 * @return      UDS_SUCCESS or error code
 **/
int destroyCond(struct cond_var *cond);


/**
 * Initialize the default type (error-checking during development) mutex.
 *
 * @param mutex the mutex to initialize
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int __must_check initMutex(struct mutex *mutex)
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
static INLINE int destroyMutex(struct mutex *mutex)
{
  return UDS_SUCCESS;
}

/**
 * Lock a mutex, with optional error checking during development.
 *
 * @param mutex mutex to lock
 **/
static INLINE void lockMutex(struct mutex *mutex)
{
  mutex_lock(mutex);
}

/**
 * Unlock a mutex, with optional error checking during development.
 *
 * @param mutex mutex to unlock
 **/
static INLINE void unlockMutex(struct mutex *mutex)
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
static INLINE int __must_check
initializeSemaphore(struct semaphore *semaphore, unsigned int value)
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
static INLINE int destroySemaphore(struct semaphore *semaphore)
{
  return UDS_SUCCESS;
}

/**
 * Acquire a permit from a semaphore, waiting if none are currently available.
 *
 * @param semaphore the semaphore to acquire
 **/
static INLINE void acquireSemaphore(struct semaphore *semaphore)
{
  // Do not use down(semaphore).  Instead use down_interruptible so that we do
  // not get 120 second stall messages in kern.log.
  while (down_interruptible(semaphore) != 0) {
    /*
     * If we're called from a user-mode process (e.g., "dmsetup remove") while
     * waiting for an operation that may take a while (e.g., UDS index save),
     * and a signal is sent (SIGINT, SIGUSR2), then down_interruptible will not
     * block. If that happens, sleep briefly to avoid keeping the CPU locked up
     * in this loop. We could just call cond_resched, but then we'd still keep
     * consuming CPU time slices and swamp other threads trying to do
     * computational work. [VDO-4980]
     */
    msleep(1);
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
static INLINE bool __must_check
attemptSemaphore(struct semaphore *semaphore, rel_time_t timeout)
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
static INLINE void releaseSemaphore(struct semaphore *semaphore)
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
