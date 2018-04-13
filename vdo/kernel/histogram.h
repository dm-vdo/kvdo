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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/histogram.h#1 $
 */

#ifndef HISTOGRAM_H
#define HISTOGRAM_H

#include <linux/types.h>

typedef struct histogram Histogram;

/**
 * Allocate and initialize a histogram that uses linearly sized buckets.
 *
 * The histogram label reported via /sys is constructed from several of the
 * values passed here; it will be something like "Init Label Histogram - number
 * of countedItems grouped by metric (sampleUnits)", e.g., "Flush Forwarding
 * Histogram - number of flushes grouped by latency (milliseconds)". Thus
 * countedItems and sampleUnits should be plural.
 *
 * The sampleUnits string will also be reported separately via another /sys
 * entry to aid in programmatic processing of the results, so the strings used
 * should be consistent (e.g., always "milliseconds" and not "ms" for
 * milliseconds).
 *
 * @param parent       The parent kobject.
 * @param name         The short name of the histogram.  This label is used
 *                     for the sysfs node.
 * @param initLabel    The label for the sampled data.  This label is used
 *                     when we plot the data.
 * @param countedItems A name (plural) for the things being counted.
 * @param metric       The measure being used to divide samples into buckets.
 * @param sampleUnits  The unit (plural) for the metric, or NULL if it's a
 *                     simple counter.
 * @param size         The number of buckets.  There are buckets for every
 *                     value from 0 up to size (but not including) size.
 *                     There is an extra bucket for larger samples.
 *
 * @return the histogram
 **/
Histogram *makeLinearHistogram(struct kobject *parent,
                               const char     *name,
                               const char     *initLabel,
                               const char     *countedItems,
                               const char     *metric,
                               const char     *sampleUnits,
                               int             size);

/**
 * Allocate and initialize a histogram that uses logarithmically sized
 * buckets.
 *
 * @param parent       The parent kobject.
 * @param name         The short name of the histogram.  This label is used
 *                     for the sysfs node.
 * @param initLabel    The label for the sampled data.  This label is used
 *                     when we plot the data.
 * @param countedItems A name (plural) for the things being counted.
 * @param metric       The measure being used to divide samples into buckets.
 * @param sampleUnits  The unit (plural) for the metric, or NULL if it's a
 *                     simple counter.
 * @param logSize      The number of buckets.  There are buckets for a range
 *                     of sizes up to 10^logSize, and an extra bucket for
 *                     larger samples.
 *
 * @return the histogram
 **/
Histogram *makeLogarithmicHistogram(struct kobject *parent,
                                    const char     *name,
                                    const char     *initLabel,
                                    const char     *countedItems,
                                    const char     *metric,
                                    const char     *sampleUnits,
                                    int             logSize);

/**
 * Allocate and initialize a histogram that uses logarithmically sized
 * buckets. Values are entered that count in jiffies, and they are
 * reported in milliseconds.
 *
 * @param parent       The parent kobject.
 * @param name         The short name of the histogram.  This label is used
 *                     for the sysfs node.
 * @param initLabel    The label for the sampled data.  This label is used
 *                     when we plot the data.
 * @param countedItems A name (plural) for the things being counted.
 * @param metric       The measure being used to divide samples into buckets.
 * @param logSize      The number of buckets.  There are buckets for a range
 *                     of sizes up to 10^logSize, and an extra bucket for
 *                     larger samples.
 *
 * @return the histogram
 **/
Histogram *makeLogarithmicJiffiesHistogram(struct kobject *parent,
                                           const char     *name,
                                           const char     *initLabel,
                                           const char     *countedItems,
                                           const char     *metric,
                                           int             logSize);

/**
 * Enter a sample into a histogram
 *
 * @param h       The histogram
 * @param sample  The sample
 **/
void enterHistogramSample(Histogram *h, uint64_t sample);

/**
 * Free a histogram and null out the reference to it.
 *
 * @param hp  The reference to the histogram.
 **/
void freeHistogram(Histogram **hp);

#endif /* HISTOGRAM_H */
