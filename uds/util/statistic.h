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
 * $Id: //eng/uds-releases/flanders/src/uds/util/statistic.h#2 $
 */

#ifndef STATISTIC_H
#define STATISTIC_H

#include "compiler.h"
#include "featureDefs.h"
#include "typeDefs.h"

#if FLOATING_POINT
#include <math.h>
#endif

/**
 * An incremental accumulator of statistical information about a sampled
 * variable.
 **/
typedef struct {
  /** the number of samples included in this statistic */
  long count;

  /** the sum of the samples included in this statistic */
  long sum;

  /** the sum of the square of every sample included in this statistic */
  long sumSquares;

  /** the minimum value of all the samples */
  long min;

  /** the maximum value of all the samples */
  long max;
} Statistic;

#define STATISTIC_INITIALIZER { 0, 0.0, 0.0, LONG_MAX, LONG_MIN }

/**
 * Initialize or reset a statistic so it contains no samples.
 *
 * @param stat  the statistic
 **/
static INLINE void resetStatistic(Statistic *stat)
{
  Statistic reset = STATISTIC_INITIALIZER;
  *stat = reset;
}

/**
 * Include a sampled value of a variable in a statistic.
 *
 * @param stat   the statistic
 * @param value  the sampled value
 **/
static INLINE void sampleStatistic(Statistic *stat, long sample)
{
  stat->count      += 1;
  stat->sum        += sample;
  stat->sumSquares += sample * sample;

  if (sample < stat->min) {
    stat->min = sample;
  }
  if (sample > stat->max) {
    stat->max = sample;
  }
}

#if FLOATING_POINT
/**
 * Return the sample mean (an average or expected value) of all the samples.
 *
 * @param stat  the statistic
 *
 * @return The arithmetic mean of the samples, or NAN if no samples have
 *         been provided.
 **/
static INLINE double getStatisticMean(const Statistic *stat)
{
  return ((double) stat->sum / stat->count);
}

/**
 * Compute a standard deviation of the samples or of the full population.
 *
 * @param stat      the statistic
 * @param estimate  <code>true</code> if the standard deviation must be
 *                  estimated based on a random sample of the population,
 *                  <code>false</code> if the entire population was captured
 *                  and can be used to calculate the exact deviation
 *
 * @return The requested standard deviation, or Double.NaN if not enough
 *         samples have been captured
 **/
static INLINE double getStatisticSigma(const Statistic *stat, bool estimate)
{
  /*
   * The sum of the squares of the deviations from an expected value is equal
   * to the sum of the squares of the values minus the sum of the squares of
   * the expected value. Note that (EV * EV * N) = (sum * sum) / N.
   *
   * If the full population was not sampled, calculate an unbiased estimate of
   * sigma by using (N - 1) in the denominator instead of N.
   */
  double sum = stat->sum;
  return sqrt((stat->sumSquares - ((sum * sum) / stat->count))
              / (estimate ? (stat->count - 1) : stat->count));
}
#endif /* FLOATING_POINT */

/**
 * Allocate a string containing a description of the samples accumulated in
 * the statistic. The caller is responsible for freeing the string.
 *
 * @param [in]  stat       the statistic
 * @param [out] stringPtr  the pointer in which to return the allocated string
 *
 * @return UDS_SUCCESS or an error code
 **/
int formatStatistic(const Statistic *stat, char **stringPtr)
  __attribute__((warn_unused_result));

/**
 * Log a message containing a captioned description of a statistic.
 *
 * @param stat      the statistic
 * @param caption   the caption string to log before the description
 **/
void logStatistic(const Statistic *stat, const char caption[]);

#endif /* STATISTIC_H */
