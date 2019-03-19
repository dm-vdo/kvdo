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
 * $Id: //eng/uds-releases/gloria/src/uds/timeUtils.h#1 $
 */

#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include "compiler.h"
#include "typeDefs.h"

// Relative time, the length of a time interval, or the difference between
// two times.  A signed 64-bit number of nanoseconds.
typedef int64_t RelTime;

// This must be included after RelTime is defined.
#include "timeDefs.h"

/**
 * Return true if the time is valid.
 *
 * @param time          a time
 * @return              true if the time is valid
 *
 * @note an invalid time is generally returned from a failed attempt
 *      to get the time from the system
 **/
bool isValidTime(AbsTime time);

/**
 * Return the current time according to the specified clock type.
 *
 * @param clock         Either CT_REALTIME or CT_MONOTONIC
 * @return the current time according to the clock in question
 *
 * @note the precision of the clock is system specific
 **/
AbsTime currentTime(ClockType clock);

/**
 * Return the timestamp a certain number of nanoseconds in the future.
 *
 * @param clock    Either CT_REALTIME or CT_MONOTONIC
 * @param reltime  The relative time to the clock value
 *
 * @return the timestamp for that time (potentially rounded to the next
 *         representable instant for the system in question)
 **/
AbsTime futureTime(ClockType clock, RelTime reltime);

/**
 * Return a time offset from the specified time.
 *
 * @param time     A time.
 * @param reltime  The relative time
 *
 * @return the sum of the time and the offset, possibly rounded up to the
 *         next representable instant.
 *
 * @note timeDifference(a, deltaTime(a, n)) may only be approx == -n
 *       depending on the system-specific time resolution
 **/
AbsTime deltaTime(AbsTime time, RelTime reltime);

/**
 * Return the difference between two timestamps.
 *
 * @param a  A time
 * @param b  Another time, based on the same clock as a.
 *
 * @return the relative time between the two timestamps
 **/
RelTime timeDifference(AbsTime a, AbsTime b);

/**
 * Convert the difference between two timestamps into a printable value.
 * The string will have the form "###.### seconds", or "###.###
 * milliseconds", or "###.### microseconds", depending upon the actual
 * value being converted.  Upon a success, the caller must FREE the string.
 *
 * @param strp     The pointer to the allocated string is returned here.
 * @param reltime  The time difference
 * @param counter  If non-zero, the reltime is divided by this value.
 *
 * @return UDS_SUCCESS or an error code
 **/
int relTimeToString(char **strp, RelTime reltime, long counter)
  __attribute__((warn_unused_result));

/**
 * Try to sleep for at least the time specified.
 *
 * @param reltime  the relative time to sleep
 **/
void sleepFor(RelTime reltime);

/**
 * Fill a buffer with the UTC time in ISO format.
 *
 * @param time          A time
 * @param buf           A buffer to fill
 * @param bufSize       The size of the buffer.
 * @param subseconds    If non-zero, how many decimal places after the
 *                      second to display.
 *
 * @result UDS_SUCCESS or an error code, particularly UDS_BUFFER_ERROR
 *         if there is insufficient space to hold the output.
 *
 * @note Output syntax is platform-dependent.
 **/
int timeInISOFormat(AbsTime       time,
                    char         *buf,
                    size_t        bufSize,
                    unsigned int  subseconds)
  __attribute__((warn_unused_result));

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

#endif /* TIME_UTILS_H */
