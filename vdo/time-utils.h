/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include "compiler.h"
#include "type-defs.h"

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
