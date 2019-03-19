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
 * $Id: //eng/uds-releases/gloria/src/uds/threads.h#2 $
 */

#ifndef THREADS_H
#define THREADS_H

#include "threadCondVar.h"
#include "threadDefs.h"
#include "threadOnce.h"
#include "threadSemaphore.h"

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
 * Yield the time slice in the given thread.
 *
 * @return UDS_SUCCESS or an error code
 **/
int yieldScheduler(void);

/**
 * Initialize a synchronous callback.
 *
 * @param callback  A synchronous callback
 *
 * @return UDS_SUCCESS or an error code
 **/
int initializeSynchronousRequest(SynchronousCallback *callback);

/**
 * Wait for a synchronous callback by waiting until the request/callback has
 * been marked as complete, then destroy the contents of the callback.
 *
 * @param callback  A synchronous callback
 **/
void awaitSynchronousRequest(SynchronousCallback *callback);

/**
 * Perform a synchronous callback by marking the request/callback as complete
 * and waking any thread waiting for completion.
 *
 * @param callback  A synchronous callback
 **/
void awakenSynchronousRequest(SynchronousCallback *callback);

#endif /* THREADS_H */
