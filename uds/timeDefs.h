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
 * $Id: //eng/uds-releases/flanders-rhel7.5/kernelLinux/uds/timeDefs.h#1 $
 */

#ifndef LINUX_KERNEL_TIME_DEFS_H
#define LINUX_KERNEL_TIME_DEFS_H

#include <linux/time.h>

typedef enum clockType {
  CT_REALTIME  = CLOCK_REALTIME,
  CT_MONOTONIC = CLOCK_MONOTONIC
} ClockType;

typedef int64_t AbsTime;

static INLINE time_t asTimeT(AbsTime time)
{
  // Convert nanoseconds to seconds
  return time / 1000000000;
}

static INLINE AbsTime fromTimeT(time_t time)
{
  return time * 1000000000;
}

#define ABSTIME_EPOCH 0

#endif // LINUX_KERNEL_TIME_DEFS_H
