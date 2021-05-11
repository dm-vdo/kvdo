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
 * $Id: //eng/uds-releases/jasper/src/uds/timeUtils.c#4 $
 */

#include "stringUtils.h"
#include "timeUtils.h"

#ifdef __KERNEL__
#include <linux/delay.h>
#include <linux/ktime.h> // for getnstimeofday on Vivid
#else
#include <errno.h>
#endif

#ifndef __KERNEL__
static const struct timespec invalidTime = {
  .tv_sec  = -1,
  .tv_nsec = LONG_MAX
};

static const long BILLION = 1000 * 1000 * 1000;
#endif

#ifndef __KERNEL__
/*****************************************************************************/
AbsTime currentTime(clockid_t clock)
{
  struct timespec ts;
  if (clock_gettime(clock, &ts) != 0) {
    ts = invalidTime;
  }
  return ts;
}
#endif

#ifndef __KERNEL__
/*****************************************************************************/
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
static AbsTime deltaTime(AbsTime time, RelTime reltime)
{
  if (!isValidTime(time)) {
    return time;
  }
  if ((reltime >= 0) && (reltime < 10 * BILLION)) {
    reltime += time.tv_nsec;
    while (reltime >= BILLION) {
      reltime -= BILLION;
      time.tv_sec++;
    }
    time.tv_nsec = reltime;
    return time;
  }
  // may not be accurate for times before the Epoch...
  // (is the ns time positive or negative for negative time_t?)
  int64_t ns = time.tv_sec * BILLION + time.tv_nsec;
  if ((ns < INT64_MIN / 2) ||
      (ns > INT64_MAX / 2) ||
      (reltime < INT64_MIN / 2) ||
      (reltime > INT64_MAX / 2)) {
    return invalidTime;
  }
  ns += reltime;
  return (AbsTime) { .tv_sec = ns / BILLION, .tv_nsec = ns % BILLION };
}
#endif

#ifndef __KERNEL__
/*****************************************************************************/
AbsTime futureTime(clockid_t clock, RelTime reltime)
{
  return deltaTime(currentTime(clock), reltime);
}
#endif

#ifndef __KERNEL__
/*****************************************************************************/
bool isValidTime(AbsTime time)
{
  if (time.tv_nsec < 0 || time.tv_nsec >= BILLION) {
    return false;
  }
  return true;
}
#endif

/*****************************************************************************/
uint64_t nowUsec(void)
{
#ifdef __KERNEL__
  static const AbsTime epoch = 0;
#else
  static const AbsTime epoch = { 0, 0 };
#endif
  return relTimeToMicroseconds(timeDifference(currentTime(CLOCK_REALTIME),
                                              epoch));
}



#ifndef __KERNEL__
/*****************************************************************************/
RelTime timeDifference(AbsTime a, AbsTime b)
{
  if (isValidTime(a) && isValidTime(b)) {
    int64_t ans = a.tv_sec * BILLION + a.tv_nsec;
    int64_t bns = b.tv_sec * BILLION + b.tv_nsec;
    return ans - bns;
  } else if (isValidTime(a)) {
    return INT64_MAX;
  } else if (isValidTime(b)) {
    return INT64_MIN;
  } else {
    return 0;
  }
}
#endif
