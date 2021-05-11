/*
 * Copyright Red Hat
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
 * $Id: //eng/uds-releases/jasper/src/uds/threads.h#4 $
 */

#ifndef THREADS_H
#define THREADS_H

#include "compiler.h"
#include "threadOnce.h"
#include "timeUtils.h"
#include "uds-error.h"

#ifdef __KERNEL__
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include "util/eventCount.h"
#else
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#endif

#ifdef __KERNEL__
typedef struct { EventCount *eventCount; } CondVar;
typedef struct mutex                       Mutex;
typedef struct semaphore                   Semaphore;
typedef struct kernelThread               *Thread;
typedef pid_t                              ThreadId;

typedef struct {
  Semaphore mutex;       // Mutex for this barrier object
  Semaphore wait;        // Semaphore for threads waiting at the barrier
  int       arrived;     // Number of threads which have arrived
  int       threadCount; // Total number of threads using this barrier
} Barrier;
#else
typedef pthread_barrier_t Barrier;
typedef pthread_cond_t    CondVar;
typedef pthread_mutex_t   Mutex;
typedef sem_t             Semaphore;
typedef pthread_t         Thread;
typedef pid_t             ThreadId;

#ifndef NDEBUG
#define MUTEX_INITIALIZER PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
#else
#define MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#endif

extern const bool DO_ASSERTIONS;
#endif

#ifdef __KERNEL__
/**
 * Apply a function to every thread that we have created.
 *
 * @param applyFunc  The function to apply
 * @param argument   The first argument to applyFunc
 *
 **/
void applyToThreads(void applyFunc(void *, struct task_struct *),
                    void *argument);
#endif

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
ThreadId getThreadId(void) __attribute__((warn_unused_result));

#ifndef __KERNEL__
/**
 * Get the name of the current thread.
 *
 * @param name   a buffer of size at least 16 to write the name to
 **/
void getThreadName(char *name);
#endif

/**
 * Wait for termination of another thread.
 *
 *
 * @param th             The thread for which to wait.
 *
 * @return               UDS_SUCCESS or error code
 **/
int joinThreads(Thread th);

#ifdef __KERNEL__
/**
 * Exit the current thread.  This is a kernel-only function that is intended to
 * be an alternative to using BUG() or BUG_ON().
 **/
__attribute__((noreturn))
void exitThread(void);
#endif

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

#ifndef __KERNEL__
/**
 * Initialize a mutex, optionally asserting if the mutex initialization fails.
 * This function should only be called directly in places where making
 * assertions is not safe.
 *
 * @param mutex         the mutex to initialize
 * @param assertOnError if <code>true</code>, an error initializing the
 *                      mutex will make an assertion
 *
 * @return UDS_SUCCESS or an error code
 **/
int initializeMutex(Mutex *mutex, bool assertOnError);
#endif

/**
 * Initialize the default type (error-checking during development) mutex.
 *
 * @param mutex the mutex to initialize
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
#ifdef __KERNEL__
static INLINE int initMutex(Mutex *mutex)
{
  mutex_init(mutex);
  return UDS_SUCCESS;
}
#else
int initMutex(Mutex *mutex);
#endif

/**
 * Destroy a mutex (with error checking during development).
 *
 * @param mutex mutex to destroy
 *
 * @return UDS_SUCCESS or error code
 **/
#ifdef __KERNEL__
static INLINE int destroyMutex(Mutex *mutex)
{
  return UDS_SUCCESS;
}
#else
int destroyMutex(Mutex *mutex);
#endif

/**
 * Lock a mutex, with optional error checking during development.
 *
 * @param mutex mutex to lock
 **/
#ifdef __KERNEL__
static INLINE void lockMutex(Mutex *mutex)
{
  mutex_lock(mutex);
}
#else
void lockMutex(Mutex *mutex);
#endif

/**
 * Unlock a mutex, with optional error checking during development.
 *
 * @param mutex mutex to unlock
 **/
#ifdef __KERNEL__
static INLINE void unlockMutex(Mutex *mutex)
{
  mutex_unlock(mutex);
}
#else
void unlockMutex(Mutex *mutex);
#endif

/**
 * Initialize a semaphore used among threads in the same process.
 *
 * @param semaphore the semaphore to initialize
 * @param value     the initial value of the semaphore
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
#ifdef __KERNEL__
static INLINE int initializeSemaphore(Semaphore *semaphore, unsigned int value)
{
  sema_init(semaphore, value);
  return UDS_SUCCESS;
}
#else
int initializeSemaphore(Semaphore *semaphore, unsigned int value);
#endif

/**
 * Destroy a semaphore used among threads in the same process.
 *
 * @param semaphore the semaphore to destroy
 *
 * @return UDS_SUCCESS or an error code
 **/
#ifdef __KERNEL__
static INLINE int destroySemaphore(Semaphore *semaphore)
{
  return UDS_SUCCESS;
}
#else
int destroySemaphore(Semaphore *semaphore);
#endif

/**
 * Acquire a permit from a semaphore, waiting if none are currently available.
 *
 * @param semaphore the semaphore to acquire
 **/
#ifdef __KERNEL__
static INLINE void acquireSemaphore(Semaphore *semaphore)
{
  // Do not use down(semaphore).  Instead use down_interruptible so that we do
  // not get 120 second stall messages in kern.log.
  while (down_interruptible(semaphore) != 0) {
  }
}
#else
void acquireSemaphore(Semaphore *semaphore);
#endif

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
#ifdef __KERNEL__
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
#else
bool attemptSemaphore(Semaphore *semaphore, RelTime timeout);
#endif

/**
 * Release a semaphore, incrementing the number of available permits.
 *
 * @param semaphore the semaphore to increment
 **/
#ifdef __KERNEL__
static INLINE void releaseSemaphore(Semaphore *semaphore)
{
  up(semaphore);
}
#else
void releaseSemaphore(Semaphore *semaphore);
#endif

/**
 * Yield the time slice in the given thread.
 *
 * @return UDS_SUCCESS or an error code
 **/
int yieldScheduler(void);

#ifndef __KERNEL__
/**
 * Allocate a thread specific key for thread specific data.
 *
 * @param key            points to location for new key
 * @param destr_function destructor function called when thread exits
 *
 * @return               UDS_SUCCESS or error code
 **/
int createThreadKey(pthread_key_t *key, void (*destr_function) (void *));

/**
 * Delete a thread specific key for thread specific data.
 *
 * @param key  key to delete
 *
 * @return     UDS_SUCCESS or error code
 **/
int deleteThreadKey(pthread_key_t key);

/**
 * Set pointer for thread specific data.
 *
 * @param key      key to be associated with pointer
 * @param pointer  data associated with key
 *
 * @return         UDS_SUCCESS or error code
 **/
int setThreadSpecific(pthread_key_t key, const void *pointer);

/**
 * Get pointer for thread specific data.
 *
 * @param key  key identifying the thread specific data
 **/
void *getThreadSpecific(pthread_key_t key);
#endif

#endif /* THREADS_H */
