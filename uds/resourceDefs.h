/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/resourceDefs.h#1 $
 */

#ifndef LINUX_KERNEL_RESOURCE_DEFS_H
#define LINUX_KERNEL_RESOURCE_DEFS_H 1

#include "compiler.h"
#include "errors.h"
#include "timeUtils.h"

typedef int ResourceUsage;
typedef struct threadStatistics ThreadStatistics;

/**
 * Free a snapshot of the system thread statistics
 *
 * @param ts  Thread statistics
 */
void freeThreadStatistics(ThreadStatistics *ts);

/**
 * Get a snapshot of the system resource usage
 *
 * @param ru  The snapshot
 *
 * @return UDS_SUCCESS or a system error code
 **/
static INLINE int getResourceUsage(ResourceUsage *ru __attribute__((unused)))
{
  return UDS_SUCCESS;
}

/**
 * Get a snapshot of the system thread statistics
 *
 * @return the system thread statistics
 **/
ThreadStatistics *getThreadStatistics(void);

/**
 * Print stats on resource usage over some interval.
 *
 * Usage:
 *   AbsTime then = currentTime(CT_REALTIME);
 *   ResourceUsage thenUsage;
 *   getResourceUsage(&thenUsage);
 *
 *   // do some stuff
 *   RelTime elapsed = timeDifference(currentTime(CT_REALTIME), then);
 *   ResourceUsage nowUsage;
 *   getResourceUsage(&nowUsage);
 *
 *   // print usage over the period.
 *   printResourceUsage(&thenUsage, &nowUsage, elapsed);
 *
 * @param prev Previously recorded resource usage
 * @param cur  Resource usage at end of interval
 * @param elapsed Length of interval
 */
static INLINE void printResourceUsage(ResourceUsage *prev
                                      __attribute__((unused)),
                                      ResourceUsage *cur
                                      __attribute__((unused)),
                                      RelTime elapsed __attribute__((unused)))
{
}

/**
 * Print stats on thread usage over some interval.
 *
 * Usage:
 *   ThreadStatistics *preThreadStats = getThreadStatistics();
 *
 *   // do some stuff
 *   ThreadStatistics *postThreadStats = getThreadStatistics();
 *
 *   // print usage over the period.
 *   printThreadStatistics(preThreadStats, postThreadStats);
 *   freeThreadStats(postThreadStats);
 *   freeThreadStats(preThreadStats);
 *
 * @param prev Thread statistics at the start of the interval
 * @param cur  Thread statistics at the end of the interval
 */
void printThreadStatistics(ThreadStatistics *prev, ThreadStatistics *cur);

/**
 * Report VM stuff of interest, to stdout.
 **/
static INLINE void printVmStuff(void)
{
}

#endif /* LINUX_KERNEL_RESOURCE_DEFS_H */
