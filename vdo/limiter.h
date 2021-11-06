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
	/* A spinlock controlling access to the contents of this struct */
	spinlock_t lock;
	/* The queue of threads waiting for a resource to become available */
	wait_queue_head_t waiter_queue;
	/* The number of resources in use */
	uint32_t active;
	/* The maximum number of resources that have ever been in use */
	uint32_t maximum;
	/* The limit to the number of resources that are allowed to be used */
	uint32_t limit;
	/* A completion waiting for the limiter to become idle */
	struct vdo_completion *completion;
};

void initialize_limiter(struct limiter *limiter, uint32_t limit);

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

void drain_vdo_limiter(struct limiter *limiter,
		       struct vdo_completion *completion);

void limiter_wait_for_one_free(struct limiter *limiter);

#endif /* LIMITER_H */
