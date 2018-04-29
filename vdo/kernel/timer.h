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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/timer.h#1 $
 */

#ifndef TIMER_H
#define TIMER_H

#include <linux/spinlock.h>
#include <linux/timer.h>

#include "kernelTypes.h"

typedef struct timer {
  struct timer_list timer;
  spinlock_t        lock;
  bool              alreadyScheduled;
  bool              shuttingDown;
  bool              initialized;
  void            (*function)(struct timer *timer);
  // No callback data pointer; embed the timer and compute an offset.
} Timer;

/**
 * Initialize but do not schedule a timer.
 *
 * @param timer     The timer
 * @param function  The work function to be invoked when the timer fires
 **/
void initTimer(Timer *timer, void (*function)(Timer *));

/**
 * Schedules the timer for the indicated time, if the timer is not
 * already scheduled to run; if it is, does nothing.
 *
 * @param timer      The timer
 * @param endTime    The time at which to fire
 *
 * @return whether the timer actually got scheduled by the
 *         current invocation
 **/
bool setTimerIfNotRunning(Timer *timer, Jiffies endTime);

/**
 * Cancel the timer, if it's been scheduled, and prevent it from being
 * scheduled again.
 *
 * @param timer     The timer
 **/
void cancelTimer(Timer *timer);

#endif /* TIMER_H */
