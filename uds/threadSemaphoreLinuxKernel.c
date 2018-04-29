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
 * $Id: //eng/uds-releases/flanders-rhel7.5/kernelLinux/uds/threadSemaphoreLinuxKernel.c#1 $
 */

#include <linux/hrtimer.h>
#include <linux/sched.h>
#include <linux/version.h>

#include "errors.h"
#include "memoryAlloc.h"
#include "threadSemaphore.h"

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,32)
#define RAW_SPIN_LOCK_INIT         raw_spin_lock_init
#define RAW_SPIN_LOCK_IRQ          raw_spin_lock_irq
#define RAW_SPIN_LOCK_IRQSAVE      raw_spin_lock_irqsave
#define RAW_SPIN_UNLOCK_IRQ        raw_spin_unlock_irq
#define RAW_SPIN_UNLOCK_IRQRESTORE raw_spin_unlock_irqrestore
#define RAW_SPINLOCK_T             raw_spinlock_t
#else
#define RAW_SPIN_LOCK_INIT         spin_lock_init
#define RAW_SPIN_LOCK_IRQ          spin_lock_irq
#define RAW_SPIN_LOCK_IRQSAVE      spin_lock_irqsave
#define RAW_SPIN_UNLOCK_IRQ        spin_unlock_irq
#define RAW_SPIN_UNLOCK_IRQRESTORE spin_unlock_irqrestore
#define RAW_SPINLOCK_T             spinlock_t
#endif

struct hr_semaphore {
  RAW_SPINLOCK_T   lock;
  unsigned int     count;
  struct list_head waitList;
};

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
  struct hr_semaphore *sem;
  int result = ALLOCATE(1, struct hr_semaphore, context, &sem);
  if (result == UDS_SUCCESS) {
    sem->count = value;
    RAW_SPIN_LOCK_INIT(&sem->lock);
    INIT_LIST_HEAD(&sem->waitList);
    semaphore->psem = sem;
  }
  return result;
}

/*****************************************************************************/
int destroySemaphore(Semaphore  *semaphore,
                     const char *context __attribute__((unused)))
{
  FREE(semaphore->psem);
  semaphore->psem = NULL;
  return UDS_SUCCESS;
}

/*****************************************************************************/
void acquireSemaphore(Semaphore  *semaphore,
                      const char *context __attribute__((unused)))
{
  struct hr_semaphore *sem = semaphore->psem;
  unsigned long flags;
  RAW_SPIN_LOCK_IRQSAVE(&sem->lock, flags);
  if (likely(sem->count > 0)) {
    sem->count--;
  } else {
    struct task_struct *task = current;
    struct semaphore_waiter waiter;
    waiter.task = task;
    waiter.up   = false;
    list_add_tail(&waiter.list, &sem->waitList);
    while (!waiter.up) {
      __set_task_state(task, TASK_INTERRUPTIBLE);
      RAW_SPIN_UNLOCK_IRQ(&sem->lock);
      schedule_timeout(MAX_SCHEDULE_TIMEOUT);
      RAW_SPIN_LOCK_IRQ(&sem->lock);
    }
  }
  RAW_SPIN_UNLOCK_IRQRESTORE(&sem->lock, flags);
}

/*****************************************************************************/
bool attemptSemaphore(Semaphore *semaphore,
                      RelTime    timeout,
                      const char *context __attribute__((unused)))
{
  struct hr_semaphore *sem = semaphore->psem;
  long hrTimeout = relTimeToNanoseconds(timeout);
  bool value = true;
  unsigned long flags;
  RAW_SPIN_LOCK_IRQSAVE(&sem->lock, flags);
  if (likely(sem->count > 0)) {
    sem->count--;
  } else if (hrTimeout <= 0) {
    value = false;
  } else {
    struct task_struct *task = current;
    struct semaphore_waiter waiter;
    waiter.task = task;
    waiter.up   = false;
    list_add_tail(&waiter.list, &sem->waitList);
    ktime_t ktime = ktime_set(0, hrTimeout);
    __set_task_state(task, TASK_UNINTERRUPTIBLE);
    RAW_SPIN_UNLOCK_IRQ(&sem->lock);
    schedule_hrtimeout(&ktime, HRTIMER_MODE_REL);
    RAW_SPIN_LOCK_IRQ(&sem->lock);
    value = waiter.up;
    if (!value) {
      list_del(&waiter.list);
    }
  }
  RAW_SPIN_UNLOCK_IRQRESTORE(&sem->lock, flags);
  return value;
}

/*****************************************************************************/
void releaseSemaphore(Semaphore  *semaphore,
                      const char *context __attribute__((unused)))
{
  struct hr_semaphore *sem = semaphore->psem;
  unsigned long flags;
  RAW_SPIN_LOCK_IRQSAVE(&sem->lock, flags);
  if (likely(list_empty(&sem->waitList))) {
    sem->count++;
  } else {
    struct semaphore_waiter *waiter
      = list_first_entry(&sem->waitList, struct semaphore_waiter, list);
    list_del(&waiter->list);
    waiter->up = true;
    wake_up_process(waiter->task);
  }
  RAW_SPIN_UNLOCK_IRQRESTORE(&sem->lock, flags);
}
