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
 */

#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include "compiler.h"
#include "typeDefs.h"

#include <linux/ktime.h>
#include <linux/time.h>

/* Some constants that are defined in kernel headers. */

/**
 * Return the current nanosecond time according to the specified clock
 * type.
 *
 * @param clock         Either CLOCK_REALTIME or CLOCK_MONOTONIC
 *
 * @return the current time according to the clock in question
 **/
static INLINE ktime_t current_time_ns(clockid_t clock)
{
	/* clock is always a constant, so gcc reduces this to a single call */
	return clock == CLOCK_MONOTONIC ? ktime_get_ns() : ktime_get_real_ns();
}






/**
 * Convert seconds to a ktime_t value
 *
 * @param seconds  A number of seconds
 *
 * @return the equivalent number of seconds as a ktime_t
 **/
static INLINE ktime_t seconds_to_ktime(int64_t seconds)
{
	return (ktime_t) seconds * NSEC_PER_SEC;
}


/**
 * Convert microseconds to a ktime_t value
 *
 * @param microseconds  A number of microseconds
 *
 * @return the equivalent number of microseconds as a ktime_t
 **/
static INLINE ktime_t us_to_ktime(int64_t microseconds)
{
	return (ktime_t) microseconds * NSEC_PER_USEC;
}

/**
 * Convert a ktime_t value to seconds
 *
 * @param reltime  The time value
 *
 * @return the equivalent number of seconds, truncated
 **/
static INLINE int64_t ktime_to_seconds(ktime_t reltime)
{
	return reltime / NSEC_PER_SEC;
}


/**
 * Return the wall clock time in microseconds. The actual value is time
 * since the epoch (see "man gettimeofday"), but the typical use is to call
 * this twice and compute the difference, giving the elapsed time between
 * the two calls.
 *
 * @return the time in microseconds
 **/
int64_t __must_check current_time_us(void);


#endif /* TIME_UTILS_H */
