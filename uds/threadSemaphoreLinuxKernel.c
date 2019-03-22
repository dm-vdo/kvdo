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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/threadSemaphoreLinuxKernel.c#1 $
 */

#include <linux/sched.h>

#include "threadSemaphore.h"

struct semaphore_waiter {
  struct list_head    list;
  struct task_struct *task;
  bool                up;
};

/*****************************************************************************/
int initializeSemaphore(Semaphore   *semaphore,
                        unsigned int value,
                        const char  *context)
{
  semaphore->count = value;
  raw_spin_lock_init(&semaphore->lock);
  INIT_LIST_HEAD(&semaphore->waitList);
  return UDS_SUCCESS;
}

/*****************************************************************************/
int destroySemaphore(Semaphore  *semaphore,
                     const char *context __attribute__((unused)))
{
  return UDS_SUCCESS;
}

/*****************************************************************************/
void acquireSemaphore(Semaphore  *semaphore,
                      const char *context __attribute__((unused)))
{
  unsigned long flags;
  raw_spin_lock_irqsave(&semaphore->lock, flags);
  if (likely(semaphore->count > 0)) {
    semaphore->count--;
  } else {
    struct task_struct *task = current;
    struct semaphore_waiter waiter;
    waiter.task = task;
    waiter.up   = false;
    list_add_tail(&waiter.list, &semaphore->waitList);
    while (!waiter.up) {
      __set_current_state(TASK_INTERRUPTIBLE);
      raw_spin_unlock_irq(&semaphore->lock);
      schedule_timeout(MAX_SCHEDULE_TIMEOUT);
      raw_spin_lock_irq(&semaphore->lock);
    }
  }
  raw_spin_unlock_irqrestore(&semaphore->lock, flags);
}

/*****************************************************************************/
bool attemptSemaphore(Semaphore *semaphore,
                      RelTime    timeout,
                      const char *context __attribute__((unused)))
{
  long hrTimeout = relTimeToNanoseconds(timeout);
  bool value = true;
  unsigned long flags;
  raw_spin_lock_irqsave(&semaphore->lock, flags);
  if (likely(semaphore->count > 0)) {
    semaphore->count--;
  } else if (hrTimeout <= 0) {
    value = false;
  } else {
    struct task_struct *task = current;
    struct semaphore_waiter waiter;
    waiter.task = task;
    waiter.up   = false;
    list_add_tail(&waiter.list, &semaphore->waitList);
    ktime_t ktime = ktime_set(0, hrTimeout);
    __set_current_state(TASK_UNINTERRUPTIBLE);
    raw_spin_unlock_irq(&semaphore->lock);
    schedule_hrtimeout(&ktime, HRTIMER_MODE_REL);
    raw_spin_lock_irq(&semaphore->lock);
    value = waiter.up;
    if (!value) {
      list_del(&waiter.list);
    }
  }
  raw_spin_unlock_irqrestore(&semaphore->lock, flags);
  return value;
}

/*****************************************************************************/
void releaseSemaphore(Semaphore  *semaphore,
                      const char *context __attribute__((unused)))
{
  unsigned long flags;
  raw_spin_lock_irqsave(&semaphore->lock, flags);
  if (likely(list_empty(&semaphore->waitList))) {
    semaphore->count++;
  } else {
    struct semaphore_waiter *waiter
      = list_first_entry(&semaphore->waitList, struct semaphore_waiter, list);
    list_del(&waiter->list);
    waiter->up = true;
    wake_up_process(waiter->task);
  }
  raw_spin_unlock_irqrestore(&semaphore->lock, flags);
}
