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
 * $Id: //eng/uds-releases/jasper/src/uds/timeUtils.h#5 $
 */

#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include "compiler.h"
#include "typeDefs.h"

#ifdef __KERNEL__
#include <linux/ktime.h>
#include <linux/time.h>
#else
#include <sys/time.h>
#include <time.h>
#endif

// Absolute time.
#ifdef __KERNEL__
typedef int64_t AbsTime;
#else
typedef struct timespec AbsTime;
#endif

// Relative time, the length of a time interval, or the difference between
// two times.  A signed 64-bit number of nanoseconds.
typedef int64_t RelTime;

#ifndef __KERNEL__
/**
 * Return true if the time is valid.
 *
 * @param time  a time
 *
 * @return true if the time is valid
 *
 * @note an invalid time is generally returned from a failed attempt
 *      to get the time from the system
 **/
bool isValidTime(AbsTime time);
#endif

/**
 * Return the current time according to the specified clock type.
 *
 * @param clock         Either CLOCK_REALTIME or CLOCK_MONOTONIC
 *
 * @return the current time according to the clock in question
 *
 * @note the precision of the clock is system specific
 **/
#ifdef __KERNEL__
static INLINE AbsTime currentTime(clockid_t clock)
{
  // clock is always a constant, so gcc reduces this to a single call
  return clock == CLOCK_MONOTONIC ? ktime_get_ns() : ktime_get_real_ns();
}
#else
AbsTime currentTime(clockid_t clock);
#endif

#ifndef __KERNEL__
/**
 * Return the timestamp a certain number of nanoseconds in the future.
 *
 * @param clock    Either CLOCK_REALTIME or CLOCK_MONOTONIC
 * @param reltime  The relative time to the clock value
 *
 * @return the timestamp for that time (potentially rounded to the next
 *         representable instant for the system in question)
 **/
AbsTime futureTime(clockid_t clock, RelTime reltime);
#endif

/**
 * Return the difference between two timestamps.
 *
 * @param a  A time
 * @param b  Another time, based on the same clock as a.
 *
 * @return the relative time between the two timestamps
 **/
#ifdef __KERNEL__
static INLINE RelTime timeDifference(AbsTime a, AbsTime b)
{
  return a - b;
}
#else
RelTime timeDifference(AbsTime a, AbsTime b);
#endif



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
uint64_t nowUsec(void) __attribute__((warn_unused_result));

/**
 * Convert from an AbsTime to a time_t
 *
 * @param time  an AbsTime time
 *
 * @return a time_t time
 **/
static INLINE time_t asTimeT(AbsTime time)
{
#ifdef __KERNEL__
  return time / 1000000000;
#else
  return time.tv_sec;
#endif
}

/**
 * Convert from a time_t to an AbsTime,
 *
 * @param time  a time_t time
 *
 * @return an AbsTime time
 **/
static INLINE AbsTime fromTimeT(time_t time)
{
#ifdef __KERNEL__
  return time * 1000000000;
#else
  AbsTime abs;
  abs.tv_sec = time;
  abs.tv_nsec = 0;
  return abs;
#endif
}

#ifndef __KERNEL__
/**
 * Convert from an AbsTime to a struct timespec
 *
 * @param time  an AbsTime time
 *
 * @return a time_t time
 **/
static INLINE struct timespec asTimeSpec(AbsTime time)
{
  return time;
}
#endif

#ifndef __KERNEL__
/**
 * Convert from an AbsTime to a struct timeval
 *
 * @param time  an AbsTime time
 *
 * @return a time_t time
 **/
static INLINE struct timeval asTimeVal(AbsTime time)
{
  struct timeval tv = { time.tv_sec, time.tv_nsec / 1000 };
  return tv;
}
#endif

#endif /* TIME_UTILS_H */
