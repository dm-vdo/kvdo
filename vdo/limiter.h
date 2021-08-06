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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/base/limiter.h#1 $
 */

#ifndef LIMITER_H
#define LIMITER_H

#include <linux/wait.h>

#include "completion.h"

/*
 * A limiter is a fancy counter used to limit resource usage.  We have a
 * limit to number of resources that we are willing to use, and a limiter
 * holds us to that limit.
 */

struct limiter {
	// A spinlock controlling access to the contents of this struct
	spinlock_t lock;
	// The queue of threads waiting for a resource to become available
	wait_queue_head_t waiter_queue;
	// The number of resources in use
	uint32_t active;
	// The maximum number number of resources that have ever been in use
	uint32_t maximum;
	// The limit to the number of resources that are allowed to be used
	uint32_t limit;
	// A completion waiting for the limiter to become idle
	struct vdo_completion *completion;
};

/**
 * Initialize a limiter structure
 *
 * @param limiter  The limiter
 * @param limit    The limit to the number of active resources
 **/
void initialize_limiter(struct limiter *limiter, uint32_t limit);

/**
 * Determine whether there are any active resources
 *
 * @param limiter  The limiter
 *
 * @return true if there are no active resources
 **/
bool limiter_is_idle(struct limiter *limiter);

/**
 * Release resources, making them available for other uses
 *
 * @param limiter  The limiter
 * @param count    The number of resources to release
 **/
void limiter_release_many(struct limiter *limiter, uint32_t count);

/**
 * Release one resource, making it available for another use
 *
 * @param limiter  The limiter
 **/
static inline void limiter_release(struct limiter *limiter)
{
	limiter_release_many(limiter, 1);
}

/**
 * Wait asynchronously for there to be no active resources.
 *
 * @param limiter     The limiter
 * @param completion  The completion to notify when the limiter is idle
 **/
void drain_vdo_limiter(struct limiter *limiter,
		       struct vdo_completion *completion);

/**
 * Prepare to start using one resource, waiting if there are too many resources
 * already in use. After returning from this routine, the caller may use the
 * resource, and must call limiter_release after freeing the resource.
 *
 * @param limiter  The limiter
 **/
void limiter_wait_for_one_free(struct limiter *limiter);

/**
 * Attempt to reserve one resource, without waiting. After returning from this
 * routine, if allocation was successful, the caller may use the resource, and
 * must call limiter_release after freeing the resource.
 *
 * @param limiter  The limiter
 *
 * @return true iff the resource was allocated
 **/
bool limiter_poll(struct limiter *limiter);

#endif /* LIMITER_H */
