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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/deadlockQueue.h#1 $
 */

#ifndef DEADLOCK_QUEUE_H
#define DEADLOCK_QUEUE_H

#include <linux/kernel.h>

#include "bio.h"

/**
 * A holding space for incoming bios if we're not able to block until VIOs
 * become available to process them.
 **/
typedef struct deadlockQueue {
  /* Protection for the other fields. */
  spinlock_t      lock;
  /* List of bios we had to accept but don't have VIOs for. */
  struct bio_list list;
  /*
   * Arrival time to use for statistics tracking for the above bios, since we
   * haven't the space to store individual arrival times for each.
   */
  Jiffies         arrivalTime;
} DeadlockQueue;

/**
 * Initialize the DeadlockQueue structure.
 *
 * @param queue  The structure to initialize
 **/
void initializeDeadlockQueue(DeadlockQueue *queue);

/**
 * Add an incoming bio to the list of saved-up bios we're not ready to start
 * processing yet.
 *
 * This excess buffering on top of what the caller implements is generally a
 * bad idea, and should be used only when necessary, such as to avoid a
 * possible deadlock situation.
 *
 * @param queue        The incoming-bio queue structure
 * @param bio          The new incoming bio to save
 * @param arrivalTime  The arrival time of this new bio
 **/
void addToDeadlockQueue(DeadlockQueue *queue, BIO *bio, Jiffies arrivalTime);

/**
 * Pull an incoming bio off the queue.
 *
 * The arrival time returned may be incorrect if multiple bios were saved, as
 * there is no per-bio storage used, only one saved arrival time for the whole
 * queue.
 *
 * @param [in]  queue        The incoming-bio queue
 * @param [out] arrivalTime  The arrival time to use for this bio
 *
 * @return  a BIO pointer, or NULL if none were queued
 **/
static inline BIO *pollDeadlockQueue(DeadlockQueue *queue,
                                     Jiffies       *arrivalTime)
{
  spin_lock(&queue->lock);
  BIO *bio = bio_list_pop(&queue->list);
  if (unlikely(bio != NULL)) {
    *arrivalTime = queue->arrivalTime;
  }
  spin_unlock(&queue->lock);
  return bio;
}

#endif // DEADLOCK_QUEUE_H
