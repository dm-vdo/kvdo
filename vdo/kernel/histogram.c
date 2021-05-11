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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/histogram.c#2 $
 */

#include <linux/kobject.h>

#include "memoryAlloc.h"
#include "typeDefs.h"

#include "histogram.h"
#include "logger.h"
#include "numUtils.h"

/*
 * Set NO_BUCKETS to streamline the histogram code by reducing it to
 * tracking just minimum, maximum, mean, etc. Only one bucket counter
 * (the final one for "bigger" values) will be used, no range checking
 * is needed to find the right bucket, and no histogram will be
 * reported. With newer compilers, the histogram output code will be
 * optimized out.
 */
enum {
  NO_BUCKETS = 1
};

/*
 * Support histogramming in the VDO code.
 *
 * This is not a complete and general histogram package.  It follows the XP
 * practice of implementing the "customer" requirements, and no more.  We can
 * support other requirements after we know what they are.
 *
 * The code was originally borrowed from Albireo, and includes both linear and
 * logarithmic histograms.  VDO only uses the logarithmic histograms.
 *
 * All samples are uint64_t values.
 *
 * A unit conversion option is supported internally to allow sample values to
 * be supplied in "jiffies" and results to be reported via /sys in
 * milliseconds. Depending on the system configuration, this could mean a
 * factor of four (a bucket for values of 1 jiffy is reported as 4-7
 * milliseconds). In theory it could be a non-integer ratio (including less
 * than one), but as the x86-64 platforms we've encountered appear to use 1 or
 * 4 milliseconds per jiffy, we don't support non-integer values yet.
 *
 * All internal processing uses the values as passed to enterHistogramSample.
 * Conversions only affect the values seen or input through the /sys interface,
 * including possibly rounding a "limit" value entered.
 */

struct histogram {
  // These fields are ordered so that enterHistogramSample touches
  // only the first cache line.
  atomic64_t     *counters;     // Counter for each bucket
  uint64_t        limit;        // We want to know how many samples are larger
  atomic64_t      sum;          // Sum of all the samples
  atomic64_t      count;        // Number of samples
  atomic64_t      minimum;      // Minimum value
  atomic64_t      maximum;      // Maximum value
  atomic64_t      unacceptable; // Number of samples that exceed the limit
  int             numBuckets;   // The number of buckets
  bool            logFlag;      // True if the y scale should be logarithmic
  // These fields are used only when reporting results.
  const char     *label;        // Histogram label
  const char     *countedItems; // Name for things being counted
  const char     *metric;       // Term for value used to divide into buckets
  const char     *sampleUnits;  // Unit for measuring metric; NULL for count
  unsigned int    conversionFactor; // Converts input units to reporting units
  struct kobject  kobj;
};

/*
 * Fixed table defining the top value for each bucket of a logarithmic
 * histogram.  We arbitrarily limit the histogram to 12 orders of magnitude.
 */
enum { MAX_LOG_SIZE = 12 };
static const uint64_t bottomValue[1 + 10 * MAX_LOG_SIZE] = {
  // 0 to 10 - The first 10 buckets are linear
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
  // 10 to 100 - From this point on, the Nth entry of the table is
  //             floor(exp10((double)N/10.0)).
  12, 15, 19, 25, 31, 39, 50, 63, 79, 100,
  // 100 to 1K
  125, 158, 199, 251, 316, 398, 501, 630, 794, 1000,
  // 1K to 10K
  1258, 1584, 1995, 2511, 3162, 3981, 5011, 6309, 7943, 10000,
  // 10K to 100K
  12589, 15848, 19952, 25118, 31622, 39810, 50118, 63095, 79432, 100000,
  // 100K to 1M
  125892, 158489, 199526, 251188, 316227,
  398107, 501187, 630957, 794328, 1000000,
  // 1M to 10M
  1258925, 1584893, 1995262, 2511886, 3162277,
  3981071, 5011872, 6309573, 7943282, 10000000,
  // 10M to 100M
  12589254, 15848931, 19952623, 25118864, 31622776,
  39810717, 50118723, 63095734, 79432823, 100000000,
  // 100M to 1G
  125892541, 158489319, 199526231, 251188643, 316227766,
  398107170, 501187233, 630957344, 794328234, 1000000000,
  // 1G to 10G
  1258925411L, 1584893192L, 1995262314L, 2511886431L, 3162277660L,
  3981071705L, 5011872336L, 6309573444L, 7943282347L, 10000000000L,
  // 10G to 100G
  12589254117L, 15848931924L, 19952623149L, 25118864315L, 31622776601L,
  39810717055L, 50118723362L, 63095734448L, 79432823472L, 100000000000L,
  // 100G to 1T
  125892541179L, 158489319246L, 199526231496L, 251188643150L, 316227766016L,
  398107170553L, 501187233627L, 630957344480L, 794328234724L, 1000000000000L,
};

/***********************************************************************/
static unsigned int divideRoundingToNearest(uint64_t number, uint64_t divisor)
{
  number += divisor / 2;
  return number / divisor;
}

/***********************************************************************/
static int maxBucket(Histogram *h)
{
  int max = h->numBuckets;
  while ((max >= 0) && (atomic64_read(&h->counters[max]) == 0)) {
    max--;
  }
  // max == -1 means that there were no samples
  return max;
}

/***********************************************************************/

typedef struct {
  struct attribute attr;
  ssize_t (*show)(Histogram *h, char *buf);
  ssize_t (*store)(Histogram *h, const char *buf, size_t length);
} HistogramAttribute;

/***********************************************************************/
static void histogramKobjRelease(struct kobject *kobj)
{
  Histogram *h = container_of(kobj, Histogram, kobj);
  FREE(h->counters);
  FREE(h);
}

/***********************************************************************/
static ssize_t histogramShow(struct kobject   *kobj,
                             struct attribute *attr,
                             char             *buf)
{
  HistogramAttribute *ha = container_of(attr, HistogramAttribute, attr);
  if (ha->show == NULL) {
    return -EINVAL;
  }
  Histogram *h = container_of(kobj, Histogram, kobj);
  return ha->show(h, buf);
}

/***********************************************************************/
static ssize_t histogramStore(struct kobject   *kobj,
                              struct attribute *attr,
                              const char       *buf,
                              size_t            length)
{
  HistogramAttribute *ha = container_of(attr, HistogramAttribute, attr);
  if (ha->show == NULL) {
    return -EINVAL;
  }
  Histogram *h = container_of(kobj, Histogram, kobj);
  return ha->store(h, buf, length);
}

/***********************************************************************/
static ssize_t histogramShowCount(Histogram *h, char *buf)
{
  int64_t count = atomic64_read(&h->count);
  return sprintf(buf, "%" PRId64 "\n", count);
}

/***********************************************************************/
static ssize_t histogramShowHistogram(Histogram *h, char *buffer)
{
  /*
   * We're given one page in which to write. The caller logs a complaint if we
   * report that we've written too much, so we'll truncate to PAGE_SIZE-1.
   */
  size_t bufferSize = PAGE_SIZE;
  bool bars = true;
  ssize_t length = 0;
  int max = maxBucket(h);
  // If max is -1, we'll fall through to reporting the total of zero.

  enum { BAR_SIZE = 50 };
  char bar[BAR_SIZE + 2];
  bar[0] = ' ';
  memset(bar + 1, '=', BAR_SIZE);
  bar[BAR_SIZE + 1] = '\0';

  uint64_t total = 0;
  for (int i = 0; i <= max; i++) {
    total += atomic64_read(&h->counters[i]);
  }

  length += snprintf(buffer, bufferSize, "%s Histogram - number of %s by %s",
                     h->label, h->countedItems, h->metric);
  if (length >= (bufferSize - 1)) {
    return bufferSize - 1;
  }
  if (h->sampleUnits != NULL) {
    length += snprintf(buffer + length, bufferSize - length, " (%s)",
                       h->sampleUnits);
    if (length >= (bufferSize - 1)) {
      return bufferSize - 1;
    }
  }
  length += snprintf(buffer + length, bufferSize - length, "\n");
  if (length >= (bufferSize - 1)) {
    return bufferSize - 1;
  }
  for (int i = 0; i <= max; i++) {
    uint64_t value = atomic64_read(&h->counters[i]);

    unsigned int barLength;
    if (bars && (total != 0)) {
      // +1 for the space at the beginning
      barLength = (divideRoundingToNearest(value * BAR_SIZE, total) + 1);
      if (barLength == 1) {
        // Don't bother printing just the initial space.
        barLength = 0;
      }
    } else {
      // 0 means skip the space and the bar
      barLength = 0;
    }

    if (h->logFlag) {
      if (i == h->numBuckets) {
        length += snprintf(buffer + length, bufferSize - length, "%-16s",
                           "Bigger");
      } else {
        unsigned int lower = h->conversionFactor * bottomValue[i];
        unsigned int upper = h->conversionFactor * bottomValue[i + 1] - 1;
        length += snprintf(buffer + length, bufferSize - length, "%6u - %7u",
                           lower, upper);
      }
    } else {
      if (i == h->numBuckets) {
        length += snprintf(buffer + length, bufferSize - length, "%6s",
                           "Bigger");
      } else {
        length += snprintf(buffer + length, bufferSize - length, "%6d", i);
      }
    }
    if (length >= (bufferSize - 1)) {
      return bufferSize - 1;
    }
    length += snprintf(buffer + length, bufferSize - length,
                       " : %12llu%.*s\n", value, barLength, bar);
    if (length >= (bufferSize - 1)) {
      return bufferSize - 1;
    }
  }

  length += snprintf(buffer + length, bufferSize - length,
                     "total %llu\n", total);
  return minSizeT(bufferSize - 1, length);
}

/***********************************************************************/
static ssize_t histogramShowMaximum(Histogram *h, char *buf)
{
  // Maximum is initialized to 0.
  unsigned long value = atomic64_read(&h->maximum);
  return sprintf(buf, "%lu\n", h->conversionFactor * value);
}

/***********************************************************************/
static ssize_t histogramShowMinimum(Histogram *h, char *buf)
{
  // Minimum is initialized to -1.
  unsigned long value = ((atomic64_read(&h->count) > 0)
                         ? atomic64_read(&h->minimum)
                         : 0);
  return sprintf(buf, "%lu\n", h->conversionFactor * value);
}

/***********************************************************************/
static ssize_t histogramShowLimit(Histogram *h, char *buf)
{
  // Display the limit in the reporting units
  return sprintf(buf, "%u\n", (unsigned int)(h->conversionFactor * h->limit));
}

/***********************************************************************/
static ssize_t histogramStoreLimit(Histogram  *h,
                                   const char *buf,
                                   size_t      length)
{
  unsigned int value;
  if ((length > 12) || (sscanf(buf, "%u", &value) != 1)) {
    return -EINVAL;
  }
  /*
   * Convert input from reporting units (e.g., milliseconds) to internal
   * recording units (e.g., jiffies).
   *
   * computeBucketCount could also be called "divideRoundingUp".
   */
  h->limit = computeBucketCount(value, h->conversionFactor);
  atomic64_set(&h->unacceptable, 0);
  return length;
}

/***********************************************************************/
static ssize_t histogramShowMean(Histogram *h, char *buf)
{
  uint64_t count = atomic64_read(&h->count);
  if (count == 0) {
    return sprintf(buf, "0/0\n");
  }
  // Compute mean, scaled up by 1000, in reporting units
  unsigned long sumTimes1000InReportingUnits
    = h->conversionFactor * atomic64_read(&h->sum) * 1000;
  unsigned int meanTimes1000
    = divideRoundingToNearest(sumTimes1000InReportingUnits, count);
  // Print mean with fractional part
  return sprintf(buf, "%u.%03u\n", meanTimes1000 / 1000,
                 meanTimes1000 % 1000);
}

/***********************************************************************/
static ssize_t histogramShowUnacceptable(Histogram *h, char *buf)
{
  int64_t count = atomic64_read(&h->unacceptable);
  return sprintf(buf, "%" PRId64 "\n", count);
}

/***********************************************************************/
static ssize_t histogramShowLabel(Histogram *h, char *buf)
{
  return sprintf(buf, "%s\n", h->label);
}

/***********************************************************************/
static ssize_t histogramShowUnit(Histogram *h, char *buf)
{
  if (h->sampleUnits != NULL) {
    return sprintf(buf, "%s\n", h->sampleUnits);
  } else {
    *buf = 0;
    return 0;
  }
}

/***********************************************************************/

static struct sysfs_ops histogramSysfsOps = {
  .show  = histogramShow,
  .store = histogramStore,
};

static HistogramAttribute countAttribute = {
  .attr = { .name = "count", .mode = 0444, },
  .show = histogramShowCount,
};

static HistogramAttribute histogramAttribute = {
  .attr = { .name = "histogram", .mode = 0444, },
  .show = histogramShowHistogram,
};

static HistogramAttribute labelAttribute = {
  .attr = { .name = "label", .mode = 0444, },
  .show = histogramShowLabel,
};

static HistogramAttribute maximumAttribute = {
  .attr = { .name = "maximum", .mode = 0444, },
  .show = histogramShowMaximum,
};

static HistogramAttribute minimumAttribute = {
  .attr = { .name = "minimum", .mode = 0444, },
  .show = histogramShowMinimum,
};

static HistogramAttribute limitAttribute = {
  .attr  = { .name = "limit", .mode = 0644, },
  .show  = histogramShowLimit,
  .store = histogramStoreLimit,
};

static HistogramAttribute meanAttribute = {
  .attr = { .name = "mean", .mode = 0444, },
  .show = histogramShowMean,
};

static HistogramAttribute unacceptableAttribute = {
  .attr = { .name = "unacceptable", .mode = 0444, },
  .show = histogramShowUnacceptable,
};

static HistogramAttribute unitAttribute = {
  .attr = { .name = "unit", .mode = 0444, },
  .show = histogramShowUnit,
};

// "Real" histogram plotting.
static struct attribute *histogramAttributes[] = {
  &countAttribute.attr,
  &histogramAttribute.attr,
  &labelAttribute.attr,
  &limitAttribute.attr,
  &maximumAttribute.attr,
  &meanAttribute.attr,
  &minimumAttribute.attr,
  &unacceptableAttribute.attr,
  &unitAttribute.attr,
  NULL,
};

static struct kobj_type histogramKobjType = {
  .release       = histogramKobjRelease,
  .sysfs_ops     = &histogramSysfsOps,
  .default_attrs = histogramAttributes,
};

static struct attribute *bucketlessHistogramAttributes[] = {
  &countAttribute.attr,
  &labelAttribute.attr,
  &maximumAttribute.attr,
  &meanAttribute.attr,
  &minimumAttribute.attr,
  &unitAttribute.attr,
  NULL,
};

static struct kobj_type bucketlessHistogramKobjType = {
  .release       = histogramKobjRelease,
  .sysfs_ops     = &histogramSysfsOps,
  .default_attrs = bucketlessHistogramAttributes,
};

/***********************************************************************/
static Histogram *makeHistogram(struct kobject *parent,
                                const char     *name,
                                const char     *label,
                                const char     *countedItems,
                                const char     *metric,
                                const char     *sampleUnits,
                                int             numBuckets,
                                unsigned long   conversionFactor,
                                bool            logFlag)
{
  Histogram *h;
  if (ALLOCATE(1, Histogram, "histogram", &h) != UDS_SUCCESS) {
    return NULL;
  }

  if (NO_BUCKETS) {
    numBuckets = 0;             // plus 1 for "bigger" bucket
  }

  if (numBuckets <= 10) {
    /*
     * The first buckets in a "logarithmic" histogram are still
     * linear, but the bucket-search mechanism is a wee bit slower
     * than for linear, so change the type.
     */
    logFlag = false;
  }

  h->label            = label;
  h->countedItems     = countedItems;
  h->metric           = metric;
  h->sampleUnits      = sampleUnits;
  h->logFlag          = logFlag;
  h->numBuckets       = numBuckets;
  h->conversionFactor = conversionFactor;
  atomic64_set(&h->minimum, -1UL);

  if (ALLOCATE(h->numBuckets + 1, atomic64_t, "histogram counters",
               &h->counters) != UDS_SUCCESS) {
    histogramKobjRelease(&h->kobj);
    return NULL;
  }

  kobject_init(&h->kobj,
               ((numBuckets > 0)
                ? &histogramKobjType
                : &bucketlessHistogramKobjType));
  if (kobject_add(&h->kobj, parent, name) != 0) {
    histogramKobjRelease(&h->kobj);
    return NULL;
  }
  return h;
}

/***********************************************************************/
Histogram *makeLinearHistogram(struct kobject *parent,
                               const char     *name,
                               const char     *initLabel,
                               const char     *countedItems,
                               const char     *metric,
                               const char     *sampleUnits,
                               int             size)
{
  return makeHistogram(parent, name, initLabel, countedItems,
                       metric, sampleUnits, size, 1, false);
}


/**
 * Intermediate routine for creating logarithmic histograms.
 *
 * Limits the histogram size, and computes the bucket count from the
 * orders-of-magnitude count.
 *
 * @param parent            The parent kobject.
 * @param name              The short name of the histogram.  This label is
 *                          used for the sysfs node.
 * @param initLabel         The label for the sampled data.  This label is used
 *                          when we plot the data.
 * @param countedItems      A name (plural) for the things being counted.
 * @param metric            The measure being used to divide samples into
 *                          buckets.
 * @param sampleUnits       The units (plural) for the metric, or NULL if it's
 *                          a simple counter.
 * @param logSize           The number of buckets.  There are buckets for a
 *                          range of sizes up to 10^logSize, and an extra
 *                          bucket for larger samples.
 * @param conversionFactor  Unit conversion factor for reporting.
 *
 * @return the histogram
 **/
static Histogram *
makeLogarithmicHistogramWithConversionFactor(struct kobject *parent,
                                             const char     *name,
                                             const char     *initLabel,
                                             const char     *countedItems,
                                             const char     *metric,
                                             const char     *sampleUnits,
                                             int             logSize,
                                             uint64_t        conversionFactor)
{
  if (logSize > MAX_LOG_SIZE) {
    logSize = MAX_LOG_SIZE;
  }
  return makeHistogram(parent, name,
                       initLabel, countedItems, metric, sampleUnits,
                       10 * logSize, conversionFactor, true);
}

/***********************************************************************/
Histogram *makeLogarithmicHistogram(struct kobject *parent,
                                    const char     *name,
                                    const char     *initLabel,
                                    const char     *countedItems,
                                    const char     *metric,
                                    const char     *sampleUnits,
                                    int             logSize)
{
  return makeLogarithmicHistogramWithConversionFactor(parent, name, initLabel,
                                                      countedItems,
                                                      metric, sampleUnits,
                                                      logSize, 1);
}

/***********************************************************************/
Histogram *makeLogarithmicJiffiesHistogram(struct kobject *parent,
                                           const char     *name,
                                           const char     *initLabel,
                                           const char     *countedItems,
                                           const char     *metric,
                                           int             logSize)
{
  /*
   * If these fail, we have a jiffy duration that is not an integral number of
   * milliseconds, and the unit conversion code needs updating.
   */
  STATIC_ASSERT(HZ <= MSEC_PER_SEC);
  STATIC_ASSERT((MSEC_PER_SEC % HZ) == 0);
  return makeLogarithmicHistogramWithConversionFactor(parent, name, initLabel,
                                                      countedItems,
                                                      metric, "milliseconds",
                                                      logSize,
                                                      jiffies_to_msecs(1));
}

/***********************************************************************/
void enterHistogramSample(Histogram *h, uint64_t sample)
{
  int bucket;
  if (h->logFlag) {
    int lo = 0;
    int hi = h->numBuckets;
    while (lo < hi) {
      int middle = (lo + hi) / 2;
      if (sample < bottomValue[middle + 1]) {
        hi = middle;
      } else {
        lo = middle + 1;
      }
    }
    bucket = lo;
  } else {
    bucket = sample < h->numBuckets ? sample : h->numBuckets;
  }
  atomic64_inc(&h->counters[bucket]);
  atomic64_inc(&h->count);
  atomic64_add(sample, &h->sum);
  if ((h->limit > 0) && (sample > h->limit)) {
    atomic64_inc(&h->unacceptable);
  }

  /*
   * Theoretically this could loop a lot; in practice it should rarely
   * do more than a single read, with no memory barrier, from a cache
   * line we've already referenced above.
   */
  uint64_t oldMaximum = atomic64_read(&h->maximum);
  while (oldMaximum < sample) {
    uint64_t readValue = atomic64_cmpxchg(&h->maximum, oldMaximum, sample);
    if (readValue == oldMaximum) {
      break;
    }
    oldMaximum = readValue;
  }

  uint64_t oldMinimum = atomic64_read(&h->minimum);
  while (oldMinimum > sample) {
    uint64_t readValue = atomic64_cmpxchg(&h->minimum, oldMinimum, sample);
    if (readValue == oldMinimum) {
      break;
    }
    oldMinimum = readValue;
  }
}

/***********************************************************************/
void freeHistogram(Histogram **hp)
{
  if (*hp != NULL) {
    Histogram *h = *hp;
    kobject_put(&h->kobj);
    *hp = NULL;
  }
}
