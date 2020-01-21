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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/limiter.h#1 $
 */

#ifndef LIMITER_H
#define LIMITER_H

#include <linux/wait.h>

/*
 * A Limiter is a fancy counter used to limit resource usage.  We have a
 * limit to number of resources that we are willing to use, and a Limiter
 * holds us to that limit.
 */

typedef struct limiter {
  // A spinlock controlling access to the contents of this struct
  spinlock_t        lock;
  // The queue of threads waiting for a resource to become available
  wait_queue_head_t waiterQueue;
  // The number of resources in use
  uint32_t          active;
  // The maximum number number of resources that have ever been in use
  uint32_t          maximum;
  // The limit to the number of resources that are allowed to be used
  uint32_t          limit;
} Limiter;

/**
 * Get the Limiter variable values (atomically under the lock)
 *
 * @param limiter  The limiter
 * @param active   The number of requests in progress
 * @param maximum  The maximum number of requests that have ever been active
 **/
void getLimiterValuesAtomically(Limiter  *limiter,
                                uint32_t *active,
                                uint32_t *maximum);

/**
 * Initialize a Limiter
 *
 * @param limiter  The limiter
 * @param limit    The limit to the number of active resources
 **/
void initializeLimiter(Limiter *limiter, uint32_t limit);

/**
 * Determine whether there are any active resources
 *
 * @param limiter  The limiter
 *
 * @return true if there are no active resources
 **/
bool limiterIsIdle(Limiter *limiter);

/**
 * Release resources, making them available for other uses
 *
 * @param limiter  The limiter
 * @param count    The number of resources to release
 **/
void limiterReleaseMany(Limiter *limiter, uint32_t count);

/**
 * Release one resource, making it available for another use
 *
 * @param limiter  The limiter
 **/
static inline void limiterRelease(Limiter *limiter)
{
  limiterReleaseMany(limiter, 1);
}

/**
 * Wait until there are no active resources
 *
 * @param limiter  The limiter
 **/
void limiterWaitForIdle(Limiter *limiter);

/**
 * Prepare to start using one resource, waiting if there are too many resources
 * already in use. After returning from this routine, the caller may use the
 * resource, and must call limiterRelease after freeing the resource.
 *
 * @param limiter  The limiter
 **/
void limiterWaitForOneFree(Limiter *limiter);

/**
 * Attempt to reserve one resource, without waiting. After returning from this
 * routine, if allocation was successful, the caller may use the resource, and
 * must call limiterRelease after freeing the resource.
 *
 * @param limiter  The limiter
 *
 * @return true iff the resource was allocated
 **/
bool limiterPoll(Limiter *limiter);

#endif /* LIMITER_H */
