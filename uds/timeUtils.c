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
 * $Id: //eng/uds-releases/gloria/src/uds/timeUtils.c#1 $
 */

#include "stringUtils.h"
#include "timeUtils.h"

/*****************************************************************************/
uint64_t nowUsec(void)
{
  static const AbsTime epoch = ABSTIME_EPOCH;
  return relTimeToMicroseconds(timeDifference(currentTime(CT_REALTIME),
                                              epoch));
}

/*****************************************************************************/
int relTimeToString(char **strp, RelTime reltime, long counter)
{
  // If there is a counter, divide the time by the counter.  This is
  // intended for reporting values of time per operation.
  RelTime rt = reltime;
  if (counter > 0) {
    rt /= counter;
  }

  const char *sign, *units;
  unsigned long value;
  if (rt < 0) {
    // Negative time is unusual, but ensure that the rest of the code
    // behaves well.
    sign = "-";
    rt = -rt;
  } else {
    sign = "";
  }
  if (rt > secondsToRelTime(1)) {
    // Larger than a second, so report to millisecond accuracy
    units = "seconds";
    value = relTimeToMilliseconds(rt);
  } else if (rt > millisecondsToRelTime(1)) {
    // Larger than a millisecond, so report to microsecond accuracy
    units = "milliseconds";
    value = relTimeToMicroseconds(rt);
  } else {
    // Larger than a microsecond, so report to nanosecond accuracy
    units = "microseconds";
    value = relTimeToNanoseconds(rt);
  }

  return allocSprintf(__func__, strp, "%s%ld.%03ld %s",
                      sign, value / 1000, value % 1000, units);
}
