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
 * $Id: //eng/uds-releases/krusty/src/uds/timeUtils.h#3 $
 */

#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include "compiler.h"
#include "typeDefs.h"

#include <linux/ktime.h>
#include <linux/time.h>

// Some constants that are defined in kernel headers.

// Absolute time.
typedef int64_t AbsTime;

// Relative time, the length of a time interval, or the difference between
// two times.  A signed 64-bit number of nanoseconds.
typedef int64_t RelTime;

/**
 * Return the current time according to the specified clock type.
 *
 * @param clock         Either CLOCK_REALTIME or CLOCK_MONOTONIC
 *
 * @return the current time according to the clock in question
 *
 * @note the precision of the clock is system specific
 **/
static INLINE AbsTime currentTime(clockid_t clock)
{
  // clock is always a constant, so gcc reduces this to a single call
  return clock == CLOCK_MONOTONIC ? ktime_get_ns() : ktime_get_real_ns();
}


/**
 * Return the difference between two timestamps.
 *
 * @param a  A time
 * @param b  Another time, based on the same clock as a.
 *
 * @return the relative time between the two timestamps
 **/
static INLINE RelTime timeDifference(AbsTime a, AbsTime b)
{
  return a - b;
}



/**
 * Convert an AbsTime value to milliseconds
 *
 * @param abstime  The absolute time
 *
 * @return the equivalent number of milliseconds since the epoch
 **/
static INLINE int64_t absTimeToMilliseconds(AbsTime abstime)
{
  return abstime / NSEC_PER_MSEC;
}

/**
 * Convert seconds to a RelTime value
 *
 * @param seconds  A number of seconds
 *
 * @return the equivalent number of seconds as a RelTime
 **/
static INLINE RelTime secondsToRelTime(int64_t seconds)
{
  return (RelTime) seconds * (1000 * 1000 * 1000);
}

/**
 * Convert milliseconds to a RelTime value
 *
 * @param milliseconds  A number of milliseconds
 *
 * @return the equivalent number of milliseconds as a RelTime
 **/
static INLINE RelTime millisecondsToRelTime(int64_t milliseconds)
{
  return (RelTime) milliseconds * (1000 * 1000);
}

/**
 * Convert microseconds to a RelTime value
 *
 * @param microseconds  A number of microseconds
 *
 * @return the equivalent number of microseconds as a RelTime
 **/
static INLINE RelTime microsecondsToRelTime(int64_t microseconds)
{
  return (RelTime) microseconds * 1000;
}

/**
 * Convert nanoseconds to a RelTime value
 *
 * @param nanoseconds  A number of nanoseconds
 *
 * @return the equivalent number of nanoseconds as a RelTime
 **/
static INLINE RelTime nanosecondsToRelTime(int64_t nanoseconds)
{
  return (RelTime) nanoseconds;
}

/**
 * Convert a RelTime value to milliseconds
 *
 * @param reltime  The relative time
 *
 * @return the equivalent number of milliseconds
 **/
static INLINE int64_t relTimeToSeconds(RelTime reltime)
{
  return reltime / (1000 * 1000 * 1000);
}

/**
 * Convert a RelTime value to milliseconds
 *
 * @param reltime  The relative time
 *
 * @return the equivalent number of milliseconds
 **/
static INLINE int64_t relTimeToMilliseconds(RelTime reltime)
{
  return reltime / (1000 * 1000);
}

/**
 * Convert a RelTime value to microseconds
 *
 * @param reltime  The relative time
 *
 * @return the equivalent number of microseconds
 **/
static INLINE int64_t relTimeToMicroseconds(RelTime reltime)
{
  return reltime / 1000;
}

/**
 * Convert a RelTime value to nanoseconds
 *
 * @param reltime  The relative time
 *
 * @return the equivalent number of nanoseconds
 **/
static INLINE int64_t relTimeToNanoseconds(RelTime reltime)
{
  return reltime;
}

/**
 * Return the wall clock time in microseconds. The actual value is time
 * since the epoch (see "man gettimeofday"), but the typical use is to call
 * this twice and compute the difference, giving the elapsed time between
 * the two calls.
 *
 * @return the time in microseconds
 **/
uint64_t __must_check nowUsec(void);

/**
 * Convert from an AbsTime to seconds truncating
 *
 * @param time  an AbsTime time
 *
 * @return a 64 bit signed number of seconds
 **/
static INLINE int64_t absTimeToSeconds(AbsTime time)
{
  return time / NSEC_PER_SEC;
}

/**
 * Convert from seconds to an AbsTime,
 *
 * @param time  a 64 bit signed number of seconds
 *
 * @return an AbsTime time
 **/
static INLINE AbsTime fromSeconds(int64_t time)
{
  return time * NSEC_PER_SEC;
}


#endif /* TIME_UTILS_H */
