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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/deadlockQueue.h#8 $
 */

#ifndef DEADLOCK_QUEUE_H
#define DEADLOCK_QUEUE_H

#include <linux/kernel.h>

#include "bio.h"

/**
 * A holding space for incoming bios if we're not able to block until VIOs
 * become available to process them.
 **/
struct deadlock_queue {
	/* Protection for the other fields. */
	spinlock_t lock;
	/* List of bios we had to accept but don't have VIOs for. */
	struct bio_list list;
	/*
	 * Arrival time to use for statistics tracking for the above
	 * bios, since we haven't the space to store individual
	 * arrival times for each.
	 */
	uint64_t arrival_jiffies;
};

/**
 * Initialize the struct deadlock_queue structure.
 *
 * @param queue  The structure to initialize
 **/
void initialize_vdo_deadlock_queue(struct deadlock_queue *queue);

/**
 * Add an incoming bio to the list of saved-up bios we're not ready to start
 * processing yet.
 *
 * This excess buffering on top of what the caller implements is generally a
 * bad idea, and should be used only when necessary, such as to avoid a
 * possible deadlock situation.
 *
 * @param queue            The incoming-bio queue structure
 * @param bio              The new incoming bio to save
 * @param arrival_jiffies  The arrival time of this new bio
 **/

void add_to_vdo_deadlock_queue(struct deadlock_queue *queue,
			       struct bio *bio,
			       uint64_t arrival_jiffies);

/**
 * Pull an incoming bio off the queue.
 *
 * The arrival time returned may be incorrect if multiple bios were saved, as
 * there is no per-bio storage used, only one saved arrival time for the whole
 * queue.
 *
 * @param [in]  queue            The incoming-bio queue
 * @param [out] arrival_jiffies  The arrival time to use for this bio
 *
 * @return a bio pointer, or NULL if none were queued
 **/
static inline struct bio *poll_vdo_deadlock_queue(struct deadlock_queue *queue,
						  uint64_t *arrival_jiffies)
{
	struct bio *bio;

	spin_lock(&queue->lock);
	bio = bio_list_pop(&queue->list);
	if (unlikely(bio != NULL)) {
		*arrival_jiffies = queue->arrival_jiffies;
	}
	spin_unlock(&queue->lock);
	return bio;
}

#endif // DEADLOCK_QUEUE_H
