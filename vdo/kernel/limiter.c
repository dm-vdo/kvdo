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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/limiter.c#2 $
 */

#include "limiter.h"

#include <linux/sched.h>

/**********************************************************************/
void getLimiterValuesAtomically(struct limiter *limiter,
                                uint32_t       *active,
                                uint32_t       *maximum)
{
  spin_lock(&limiter->lock);
  *active  = limiter->active;
  *maximum = limiter->maximum;
  spin_unlock(&limiter->lock);
}

/**********************************************************************/
void initializeLimiter(struct limiter *limiter, uint32_t limit)
{
  limiter->active  = 0;
  limiter->limit   = limit;
  limiter->maximum = 0;
  init_waitqueue_head(&limiter->waiterQueue);
  spin_lock_init(&limiter->lock);
}

/**********************************************************************/
bool limiterIsIdle(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  bool idle = limiter->active == 0;
  spin_unlock(&limiter->lock);
  return idle;
}

/**********************************************************************/
bool limiterHasOneFree(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  bool hasOneFree = (limiter->active < limiter->limit);
  spin_unlock(&limiter->lock);
  return hasOneFree;
}

/**********************************************************************/
void limiterReleaseMany(struct limiter *limiter, uint32_t count)
{
  spin_lock(&limiter->lock);
  limiter->active -= count;
  spin_unlock(&limiter->lock);
  if (waitqueue_active(&limiter->waiterQueue)) {
    wake_up_nr(&limiter->waiterQueue, count);
  }
}

/**********************************************************************/
void limiterWaitForIdle(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  while (limiter->active > 0) {
    DEFINE_WAIT(wait);
    prepare_to_wait_exclusive(&limiter->waiterQueue, &wait,
                              TASK_UNINTERRUPTIBLE);
    spin_unlock(&limiter->lock);
    io_schedule();
    spin_lock(&limiter->lock);
    finish_wait(&limiter->waiterQueue, &wait);
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
static bool takePermitLocked(struct limiter *limiter)
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
void limiterWaitForOneFree(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  while (!takePermitLocked(limiter)) {
    DEFINE_WAIT(wait);
    prepare_to_wait_exclusive(&limiter->waiterQueue, &wait,
                              TASK_UNINTERRUPTIBLE);
    spin_unlock(&limiter->lock);
    io_schedule();
    spin_lock(&limiter->lock);
    finish_wait(&limiter->waiterQueue, &wait);
  };
  spin_unlock(&limiter->lock);
}

/**********************************************************************/
bool limiterPoll(struct limiter *limiter)
{
  spin_lock(&limiter->lock);
  bool acquired = takePermitLocked(limiter);
  spin_unlock(&limiter->lock);
  return acquired;
}
