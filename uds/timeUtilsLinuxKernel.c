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
 * $Id: //eng/uds-releases/homer/kernelLinux/uds/timeUtilsLinuxKernel.c#1 $
 */

#include <linux/delay.h>
#include <linux/ktime.h> // for getnstimeofday on Vivid

#include "errors.h"
#include "stringUtils.h"
#include "timeUtils.h"

/*****************************************************************************/
AbsTime currentTime(ClockType clock __attribute__((unused)))
{
  struct timespec now;
  getnstimeofday(&now);
  return 1000000000ul * now.tv_sec + now.tv_nsec;
}

/*****************************************************************************/
AbsTime futureTime(ClockType clock, RelTime reltime)
{
  return deltaTime(currentTime(clock), reltime);
}

/*****************************************************************************/
AbsTime deltaTime(AbsTime time, RelTime reltime)
{
  return time + reltime;
}

/*****************************************************************************/
RelTime timeDifference(AbsTime a, AbsTime b)
{
  return a - b;
}

/*****************************************************************************/
void sleepFor(RelTime reltime)
{
  unsigned long rt = 1 + relTimeToMicroseconds(reltime);
  usleep_range(rt, rt);
}

/*****************************************************************************/
static long roundToSubseconds(long nsec, unsigned int subseconds)
{
  int exp = (subseconds > 9) ? 0 : 9 - subseconds;
  long div = 1;
  while (exp > 0) {
    div *= 10;
    --exp;
  }
  return nsec / div;
}

/*****************************************************************************/
int timeInISOFormat(AbsTime       time,
                    char         *buf,
                    size_t        bufSize,
                    unsigned int  subseconds)
{
  char *bp = buf;
  char *be = buf + bufSize;
  long seconds = asTimeT(time);
  if (subseconds) {
    bp = appendToBuffer(bp, be, "[%ld.%0*ld]", seconds, subseconds,
                        roundToSubseconds(time, subseconds));
  } else {
    bp = appendToBuffer(bp, be, "[%ld]", seconds);
  }
  return (bp < be) ? UDS_SUCCESS : UDS_BUFFER_ERROR;
}
