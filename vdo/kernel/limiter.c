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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/limiter.c#3 $
 */

#include "limiter.h"

#include <linux/sched.h>

/**********************************************************************/
void get_limiter_values_atomically(struct limiter *limiter,
                                   uint32_t       *active,
                                   uint32_t       *maximum)
{
  spin_lock(&limiter->lock);
  *active  = limiter->active;
  *maximum = limiter->maximum;
  spin_unlock(&limiter->lock);
}

/**********************************************************************/
void initialize_limiter(struct limiter *limiter, uint32_t limit)
{
  limiter->active  = 0;
  limiter->limit   = limit;
  limiter->maximum = 0;
  init_waitqueue_head(&limiter->waiter_queue);
  spin_lock_init(&limiter->lock);
}

/**********************************************************************/
bool limiter_is_idle(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  bool idle = limiter->active == 0;
  spin_unlock(&limiter->lock);
  return idle;
}

/**********************************************************************/
bool limiter_has_one_free(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  bool has_one_free = (limiter->active < limiter->limit);
  spin_unlock(&limiter->lock);
  return has_one_free;
}

/**********************************************************************/
void limiter_release_many(struct limiter *limiter, uint32_t count)
{
  spin_lock(&limiter->lock);
  limiter->active -= count;
  spin_unlock(&limiter->lock);
  if (waitqueue_active(&limiter->waiter_queue)) {
    wake_up_nr(&limiter->waiter_queue, count);
  }
}

/**********************************************************************/
void limiter_wait_for_idle(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  while (limiter->active > 0) {
    DEFINE_WAIT(wait);
    prepare_to_wait_exclusive(&limiter->waiter_queue, &wait,
                              TASK_UNINTERRUPTIBLE);
    spin_unlock(&limiter->lock);
    io_schedule();
    spin_lock(&limiter->lock);
    finish_wait(&limiter->waiter_queue, &wait);
  };
  spin_unlock(&limiter->lock);
}

/**
 * Take one permit from the limiter, if one is available, and update
 * the maximum active count if appropriate.
 *
 * The limiter's lock must already be locked.
 *
 * @param limiter  The limiter to update
 *
 * @return  true iff the permit was acquired
 **/
static bool take_permit_locked(struct limiter *limiter)
{
  if (limiter->active >= limiter->limit) {
    return false;
  }
  limiter->active += 1;
  if (limiter->active > limiter->maximum) {
    limiter->maximum = limiter->active;
  }
  return true;
}

/**********************************************************************/
void limiter_wait_for_one_free(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  while (!take_permit_locked(limiter)) {
    DEFINE_WAIT(wait);
    prepare_to_wait_exclusive(&limiter->waiter_queue, &wait,
                              TASK_UNINTERRUPTIBLE);
    spin_unlock(&limiter->lock);
    io_schedule();
    spin_lock(&limiter->lock);
    finish_wait(&limiter->waiter_queue, &wait);
  };
  spin_unlock(&limiter->lock);
}

/**********************************************************************/
bool limiter_poll(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  bool acquired = take_permit_locked(limiter);
  spin_unlock(&limiter->lock);
  return acquired;
}
