/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/src/uds/util/statistic.c#2 $
 */

#include "statistic.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"
#include "uds.h"

/**********************************************************************/
int formatStatistic(const Statistic *stat, char **stringPtr)
{
  if (stat->count == 0) {
    return allocSprintf(__func__, stringPtr,
                        "no samples for computing statistics");
  } else if (stat->count == 1) {
    return allocSprintf(__func__, stringPtr, "one sample = %ld", stat->sum);
  } else {
#if FLOATING_POINT
    return allocSprintf(__func__, stringPtr,
                        "%ld samples in [%ld .. %ld] %f +- %f",
                        stat->count, stat->min, stat->max,
                        getStatisticMean(stat), getStatisticSigma(stat, false));
#else
    return allocSprintf(__func__, stringPtr,
                        "%ld samples in [%ld .. %ld] sum %ld sumsq %ld",
                        stat->count, stat->min, stat->max, stat->sum,
                        stat->sumSquares);
#endif /* FLOATING_POINT */
  }
}

/**********************************************************************/
void logStatistic(const Statistic *stat, const char caption[])
{
  if (stat->count > 0) {
    char *statString;
    int result = formatStatistic(stat, &statString);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "Failed to format %s", caption);
      return;
    }
    logInfo("%s: %s", caption, statString);
    FREE(statString);
  }
}
