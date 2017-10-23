/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/timer.c#1 $
 */

#include "timer.h"

/**
 * Work function for the low-level Linux timer.
 *
 * @param data    Callback data, holding the address of the Timer structure
 **/
/**********************************************************************/
static void timerWorkFunction(unsigned long data)
{
  Timer *timer = (void *)data;
  unsigned long flags;
  spin_lock_irqsave(&timer->lock, flags);
  // Allow the real work function to reschedule itself now.
  timer->alreadyScheduled = false;
  spin_unlock_irqrestore(&timer->lock, flags);
  timer->function(timer);
}

/**********************************************************************/
void initTimer(Timer *timer, void (*function)(Timer *))
{
  timer->function = function;
  timer->alreadyScheduled = false;
  setup_timer(&timer->timer, timerWorkFunction, (unsigned long) timer);
  spin_lock_init(&timer->lock);
  timer->initialized = true;
}

/**********************************************************************/
void cancelTimer(Timer *timer)
{
  if (!timer->initialized) {
    return;
  }

  unsigned long flags;
  spin_lock_irqsave(&timer->lock, flags);
  timer->shuttingDown = true;
  timer->alreadyScheduled = false;
  spin_unlock_irqrestore(&timer->lock, flags);
  // Release the lock first in case the work function is running and
  // trying to reschedule itself!
  del_timer_sync(&timer->timer);
}

/**********************************************************************/
bool setTimerIfNotRunning(Timer *timer, Jiffies endTimeJiffies)
{
  bool result = false;
  unsigned long flags;
  spin_lock_irqsave(&timer->lock, flags);
  if (!timer->alreadyScheduled && !timer->shuttingDown) {
    mod_timer(&timer->timer, endTimeJiffies);
    timer->alreadyScheduled = true;
    result = true;
  }
  spin_unlock_irqrestore(&timer->lock, flags);
  return result;
}
