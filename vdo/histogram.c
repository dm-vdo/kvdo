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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/histogram.c#4 $
 */

#include <linux/kobject.h>

#include "memoryAlloc.h"
#include "typeDefs.h"
#include "permassert.h"

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
 * The code was originally borrowed from UDS, and includes both linear and
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
 * All internal processing uses the values as passed to enter_histogram_sample.
 * Conversions only affect the values seen or input through the /sys interface,
 * including possibly rounding a "limit" value entered.
 */

struct histogram {
	// These fields are ordered so that enter_histogram_sample touches
	// only the first cache line.
	atomic64_t *counters; // Counter for each bucket
	uint64_t limit; // We want to know how many samples are
			//   larger
	atomic64_t sum; // Sum of all the samples
	atomic64_t count; // Number of samples
	atomic64_t minimum; // Minimum value
	atomic64_t maximum; // Maximum value
	atomic64_t unacceptable; // Number of samples that exceed the limit
	int num_buckets; // The number of buckets
	bool log_flag; // True if the y scale should be logarithmic
	// These fields are used only when reporting results.
	const char *label; // Histogram label
	const char *counted_items; // Name for things being counted
	const char *metric; // Term for value used to divide into buckets
	const char *sample_units; // Unit for measuring metric; NULL for count
	unsigned int conversion_factor; // Converts input units to reporting
					// units
	struct kobject kobj;
};

/*
 * Fixed table defining the top value for each bucket of a logarithmic
 * histogram.  We arbitrarily limit the histogram to 12 orders of magnitude.
 */
enum { MAX_LOG_SIZE = 12 };
static const uint64_t bottom_value[1 + 10 * MAX_LOG_SIZE] = {
	// 0 to 10 - The first 10 buckets are linear
	0,
	1,
	2,
	3,
	4,
	5,
	6,
	7,
	8,
	9,
	10,
	// 10 to 100 - From this point on, the Nth entry of the table is
	//             floor(exp10((double) N/10.0)).
	12,
	15,
	19,
	25,
	31,
	39,
	50,
	63,
	79,
	100,
	// 100 to 1K
	125,
	158,
	199,
	251,
	316,
	398,
	501,
	630,
	794,
	1000,
	// 1K to 10K
	1258,
	1584,
	1995,
	2511,
	3162,
	3981,
	5011,
	6309,
	7943,
	10000,
	// 10K to 100K
	12589,
	15848,
	19952,
	25118,
	31622,
	39810,
	50118,
	63095,
	79432,
	100000,
	// 100K to 1M
	125892,
	158489,
	199526,
	251188,
	316227,
	398107,
	501187,
	630957,
	794328,
	1000000,
	// 1M to 10M
	1258925,
	1584893,
	1995262,
	2511886,
	3162277,
	3981071,
	5011872,
	6309573,
	7943282,
	10000000,
	// 10M to 100M
	12589254,
	15848931,
	19952623,
	25118864,
	31622776,
	39810717,
	50118723,
	63095734,
	79432823,
	100000000,
	// 100M to 1G
	125892541,
	158489319,
	199526231,
	251188643,
	316227766,
	398107170,
	501187233,
	630957344,
	794328234,
	1000000000,
	// 1G to 10G
	1258925411L,
	1584893192L,
	1995262314L,
	2511886431L,
	3162277660L,
	3981071705L,
	5011872336L,
	6309573444L,
	7943282347L,
	10000000000L,
	// 10G to 100G
	12589254117L,
	15848931924L,
	19952623149L,
	25118864315L,
	31622776601L,
	39810717055L,
	50118723362L,
	63095734448L,
	79432823472L,
	100000000000L,
	// 100G to 1T
	125892541179L,
	158489319246L,
	199526231496L,
	251188643150L,
	316227766016L,
	398107170553L,
	501187233627L,
	630957344480L,
	794328234724L,
	1000000000000L,
};

/***********************************************************************/
static unsigned int divide_rounding_to_nearest(uint64_t number,
					       uint64_t divisor)
{
	number += divisor / 2;
	return number / divisor;
}

/***********************************************************************/
static int max_bucket(struct histogram *h)
{
	int max = h->num_buckets;

	while ((max >= 0) && (atomic64_read(&h->counters[max]) == 0)) {
		max--;
	}
	// max == -1 means that there were no samples
	return max;
}

/***********************************************************************/

struct histogram_attribute {
	struct attribute attr;
	ssize_t (*show)(struct histogram *h, char *buf);
	ssize_t (*store)(struct histogram *h, const char *buf, size_t length);
};

/***********************************************************************/
static void histogram_kobj_release(struct kobject *kobj)
{
	struct histogram *h = container_of(kobj, struct histogram, kobj);

	UDS_FREE(h->counters);
	UDS_FREE(h);
}

/***********************************************************************/
static ssize_t histogram_show(struct kobject *kobj,
			      struct attribute *attr,
			      char *buf)
{
	struct histogram_attribute *ha =
		container_of(attr, struct histogram_attribute, attr);
	struct histogram *h = container_of(kobj, struct histogram, kobj);
	if (ha->show == NULL) {
		return -EINVAL;
	}

	return ha->show(h, buf);
}

/***********************************************************************/
static ssize_t histogram_store(struct kobject *kobj,
			       struct attribute *attr,
			       const char *buf,
			       size_t length)
{
	struct histogram_attribute *ha =
		container_of(attr, struct histogram_attribute, attr);
	struct histogram *h = container_of(kobj, struct histogram, kobj);

	if (ha->show == NULL) {
		return -EINVAL;
	}

	return ha->store(h, buf, length);
}

/***********************************************************************/
static ssize_t histogram_show_count(struct histogram *h, char *buf)
{
	int64_t count = atomic64_read(&h->count);

	return sprintf(buf, "%lld\n", count);
}

/***********************************************************************/
static ssize_t histogram_show_histogram(struct histogram *h, char *buffer)
{
	/*
	 * We're given one page in which to write. The caller logs a complaint
	 * if we report that we've written too much, so we'll truncate to
	 * PAGE_SIZE-1.
	 */
	ssize_t buffer_size = PAGE_SIZE;
	bool bars = true;
	ssize_t length = 0;
	int max = max_bucket(h);
	uint64_t total = 0;
	int i;

	// If max is -1, we'll fall through to reporting the total of zero.

	enum { BAR_SIZE = 50 };
	char bar[BAR_SIZE + 2];

	bar[0] = ' ';
	memset(bar + 1, '=', BAR_SIZE);
	bar[BAR_SIZE + 1] = '\0';

	for (i = 0; i <= max; i++) {
		total += atomic64_read(&h->counters[i]);
	}

	length += scnprintf(buffer,
			    buffer_size,
			    "%s Histogram - number of %s by %s",
			    h->label,
			    h->counted_items,
			    h->metric);
	if (length >= (buffer_size - 1)) {
		return buffer_size - 1;
	}
	if (h->sample_units != NULL) {
		length += scnprintf(buffer + length,
				    buffer_size - length,
				    " (%s)",
				    h->sample_units);
		if (length >= (buffer_size - 1)) {
			return buffer_size - 1;
		}
	}
	length += scnprintf(buffer + length, buffer_size - length, "\n");
	if (length >= (buffer_size - 1)) {
		return buffer_size - 1;
	}
	for (i = 0; i <= max; i++) {
		uint64_t value = atomic64_read(&h->counters[i]);

		unsigned int bar_length;

		if (bars && (total != 0)) {
			// +1 for the space at the beginning
			bar_length = (divide_rounding_to_nearest(
					      value * BAR_SIZE, total) +
				      1);
			if (bar_length == 1) {
				// Don't bother printing just the initial space.
				bar_length = 0;
			}
		} else {
			// 0 means skip the space and the bar
			bar_length = 0;
		}

		if (h->log_flag) {
			if (i == h->num_buckets) {
				length += scnprintf(buffer + length,
						    buffer_size - length,
						    "%-16s",
						    "Bigger");
			} else {
				unsigned int lower =
					h->conversion_factor * bottom_value[i];
				unsigned int upper =
					h->conversion_factor *
						bottom_value[i + 1] -
					1;
				length += scnprintf(buffer + length,
						    buffer_size - length,
						    "%6u - %7u",
						    lower,
						    upper);
			}
		} else {
			if (i == h->num_buckets) {
				length += scnprintf(buffer + length,
						    buffer_size - length,
						    "%6s",
						    "Bigger");
			} else {
				length += scnprintf(buffer + length,
						    buffer_size - length,
						    "%6d",
						    i);
			}
		}
		if (length >= (buffer_size - 1)) {
			return buffer_size - 1;
		}
		length += scnprintf(buffer + length,
				    buffer_size - length,
				    " : %12llu%.*s\n",
				    value,
				    bar_length,
				    bar);
		if (length >= (buffer_size - 1)) {
			return buffer_size - 1;
		}
	}

	length += scnprintf(buffer + length,
			    buffer_size - length,
			    "total %llu\n",
			    total);
	return min(buffer_size - 1, length);
}

/***********************************************************************/
static ssize_t histogram_show_maximum(struct histogram *h, char *buf)
{
	// Maximum is initialized to 0.
	unsigned long value = atomic64_read(&h->maximum);

	return sprintf(buf, "%lu\n", h->conversion_factor * value);
}

/***********************************************************************/
static ssize_t histogram_show_minimum(struct histogram *h, char *buf)
{
	// Minimum is initialized to -1.
	unsigned long value =
		((atomic64_read(&h->count) > 0) ? atomic64_read(&h->minimum) :
						  0);
	return sprintf(buf, "%lu\n", h->conversion_factor * value);
}

/***********************************************************************/
static ssize_t histogram_show_limit(struct histogram *h, char *buf)
{
	// Display the limit in the reporting units
	return sprintf(buf,
		       "%u\n",
		       (unsigned int) (h->conversion_factor * h->limit));
}

/***********************************************************************/
static ssize_t histogram_store_limit(struct histogram *h,
				     const char *buf,
				     size_t length)
{
	unsigned int value;

	if ((length > 12) || (sscanf(buf, "%u", &value) != 1)) {
		return -EINVAL;
	}
	/*
	 * Convert input from reporting units (e.g., milliseconds) to internal
	 * recording units (e.g., jiffies).
	 *
	 * compute_bucket_count could also be called "divide_rounding_up".
	 */
	h->limit = compute_bucket_count(value, h->conversion_factor);
	atomic64_set(&h->unacceptable, 0);
	return length;
}

/***********************************************************************/
static ssize_t histogram_show_mean(struct histogram *h, char *buf)
{
	unsigned long sum_times1000_in_reporting_units;
	unsigned int mean_times1000;
	uint64_t count = atomic64_read(&h->count);

	if (count == 0) {
		return sprintf(buf, "0/0\n");
	}
	// Compute mean, scaled up by 1000, in reporting units
	sum_times1000_in_reporting_units =
		h->conversion_factor * atomic64_read(&h->sum) * 1000;
	mean_times1000 = divide_rounding_to_nearest(
		sum_times1000_in_reporting_units, count);
	// Print mean with fractional part
	return sprintf(buf,
		       "%u.%03u\n",
		       mean_times1000 / 1000,
		       mean_times1000 % 1000);
}

/***********************************************************************/
static ssize_t histogram_show_unacceptable(struct histogram *h, char *buf)
{
	int64_t count = atomic64_read(&h->unacceptable);

	return sprintf(buf, "%lld\n", count);
}

/***********************************************************************/
static ssize_t histogram_show_label(struct histogram *h, char *buf)
{
	return sprintf(buf, "%s\n", h->label);
}

/***********************************************************************/
static ssize_t histogram_show_unit(struct histogram *h, char *buf)
{
	if (h->sample_units != NULL) {
		return sprintf(buf, "%s\n", h->sample_units);
	} else {
		*buf = 0;
		return 0;
	}
}

/***********************************************************************/

static struct sysfs_ops histogram_sysfs_ops = {
	.show = histogram_show,
	.store = histogram_store,
};

static struct histogram_attribute count_attribute = {
	.attr = {
			.name = "count",
			.mode = 0444,
		},
	.show = histogram_show_count,
};

static struct histogram_attribute histogram_attribute = {
	.attr = {
			.name = "histogram",
			.mode = 0444,
		},
	.show = histogram_show_histogram,
};

static struct histogram_attribute label_attribute = {
	.attr = {
			.name = "label",
			.mode = 0444,
		},
	.show = histogram_show_label,
};

static struct histogram_attribute maximum_attribute = {
	.attr = {
			.name = "maximum",
			.mode = 0444,
		},
	.show = histogram_show_maximum,
};

static struct histogram_attribute minimum_attribute = {
	.attr = {
			.name = "minimum",
			.mode = 0444,
		},
	.show = histogram_show_minimum,
};

static struct histogram_attribute limit_attribute = {
	.attr = {
			.name = "limit",
			.mode = 0644,
		},
	.show = histogram_show_limit,
	.store = histogram_store_limit,
};

static struct histogram_attribute mean_attribute = {
	.attr = {
			.name = "mean",
			.mode = 0444,
		},
	.show = histogram_show_mean,
};

static struct histogram_attribute unacceptable_attribute = {
	.attr = {
			.name = "unacceptable",
			.mode = 0444,
		},
	.show = histogram_show_unacceptable,
};

static struct histogram_attribute unit_attribute = {
	.attr = {
			.name = "unit",
			.mode = 0444,
		},
	.show = histogram_show_unit,
};

// "Real" histogram plotting.
static struct attribute *histogram_attributes[] = {
	&count_attribute.attr,
	&histogram_attribute.attr,
	&label_attribute.attr,
	&limit_attribute.attr,
	&maximum_attribute.attr,
	&mean_attribute.attr,
	&minimum_attribute.attr,
	&unacceptable_attribute.attr,
	&unit_attribute.attr,
	NULL,
};

static struct kobj_type histogram_kobj_type = {
	.release = histogram_kobj_release,
	.sysfs_ops = &histogram_sysfs_ops,
	.default_attrs = histogram_attributes,
};

static struct attribute *bucketless_histogram_attributes[] = {
	&count_attribute.attr,
	&label_attribute.attr,
	&maximum_attribute.attr,
	&mean_attribute.attr,
	&minimum_attribute.attr,
	&unit_attribute.attr,
	NULL,
};

static struct kobj_type bucketless_histogram_kobj_type = {
	.release = histogram_kobj_release,
	.sysfs_ops = &histogram_sysfs_ops,
	.default_attrs = bucketless_histogram_attributes,
};

/***********************************************************************/
static struct histogram *make_histogram(struct kobject *parent,
					const char *name,
					const char *label,
					const char *counted_items,
					const char *metric,
					const char *sample_units,
					int num_buckets,
					unsigned long conversion_factor,
					bool log_flag)
{
	struct histogram *h;

	if (UDS_ALLOCATE(1, struct histogram, "histogram", &h) != UDS_SUCCESS) {
		return NULL;
	}

	if (NO_BUCKETS) {
		num_buckets = 0; // plus 1 for "bigger" bucket
	}

	if (num_buckets <= 10) {
		/*
		 * The first buckets in a "logarithmic" histogram are still
		 * linear, but the bucket-search mechanism is a wee bit slower
		 * than for linear, so change the type.
		 */
		log_flag = false;
	}

	h->label = label;
	h->counted_items = counted_items;
	h->metric = metric;
	h->sample_units = sample_units;
	h->log_flag = log_flag;
	h->num_buckets = num_buckets;
	h->conversion_factor = conversion_factor;
	atomic64_set(&h->minimum, -1UL);

	if (UDS_ALLOCATE(h->num_buckets + 1,
			 atomic64_t,
			 "histogram counters",
			 &h->counters) != UDS_SUCCESS) {
		histogram_kobj_release(&h->kobj);
		return NULL;
	}

	kobject_init(&h->kobj,
		     ((num_buckets > 0) ? &histogram_kobj_type :
					  &bucketless_histogram_kobj_type));
	if (kobject_add(&h->kobj, parent, name) != 0) {
		histogram_kobj_release(&h->kobj);
		return NULL;
	}
	return h;
}

/***********************************************************************/
struct histogram *make_linear_histogram(struct kobject *parent,
					const char *name,
					const char *init_label,
					const char *counted_items,
					const char *metric,
					const char *sample_units,
					int size)
{
	return make_histogram(parent,
			      name,
			      init_label,
			      counted_items,
			      metric,
			      sample_units,
			      size,
			      1,
			      false);
}

/**
 * Intermediate routine for creating logarithmic histograms.
 *
 * Limits the histogram size, and computes the bucket count from the
 * orders-of-magnitude count.
 *
 * @param parent             The parent kobject.
 * @param name               The short name of the histogram.  This label is
 *                           used for the sysfs node.
 * @param init_label         The label for the sampled data.  This label is
 *                           used when we plot the data.
 * @param counted_items      A name (plural) for the things being counted.
 * @param metric             The measure being used to divide samples into
 *                           buckets.
 * @param sample_units       The units (plural) for the metric, or NULL if it's
 *                           a simple counter.
 * @param log_size           The number of buckets.  There are buckets for a
 *                           range of sizes up to 10^log_size, and an extra
 *                           bucket for larger samples.
 * @param conversion_factor  Unit conversion factor for reporting.
 *
 * @return the histogram
 **/
static struct histogram *
make_logarithmic_histogram_with_conversion_factor(struct kobject *parent,
						  const char *name,
						  const char *init_label,
						  const char *counted_items,
						  const char *metric,
						  const char *sample_units,
						  int log_size,
						  uint64_t conversion_factor)
{
	if (log_size > MAX_LOG_SIZE) {
		log_size = MAX_LOG_SIZE;
	}
	return make_histogram(parent,
			      name,
			      init_label,
			      counted_items,
			      metric,
			      sample_units,
			      10 * log_size,
			      conversion_factor,
			      true);
}

/***********************************************************************/
struct histogram *make_logarithmic_histogram(struct kobject *parent,
					     const char *name,
					     const char *init_label,
					     const char *counted_items,
					     const char *metric,
					     const char *sample_units,
					     int log_size)
{
	return make_logarithmic_histogram_with_conversion_factor(parent,
								 name,
								 init_label,
								 counted_items,
								 metric,
								 sample_units,
								 log_size,
								 1);
}

/***********************************************************************/
struct histogram *make_logarithmic_jiffies_histogram(struct kobject *parent,
						     const char *name,
						     const char *init_label,
						     const char *counted_items,
						     const char *metric,
						     int log_size)
{
	/*
	 * If these fail, we have a jiffy duration that is not an integral
	 * number of milliseconds, and the unit conversion code needs updating.
	 */
	STATIC_ASSERT(HZ <= MSEC_PER_SEC);
	STATIC_ASSERT((MSEC_PER_SEC % HZ) == 0);
	return make_logarithmic_histogram_with_conversion_factor(
		parent,
		name,
		init_label,
		counted_items,
		metric,
		"milliseconds",
		log_size,
		jiffies_to_msecs(1));
}

/***********************************************************************/
void enter_histogram_sample(struct histogram *h, uint64_t sample)
{
	uint64_t old_minimum, old_maximum;
	int bucket;

	if (h == NULL) {
		return;
	}

	if (h->log_flag) {
		int lo = 0;
		int hi = h->num_buckets;

		while (lo < hi) {
			int middle = (lo + hi) / 2;

			if (sample < bottom_value[middle + 1]) {
				hi = middle;
			} else {
				lo = middle + 1;
			}
		}
		bucket = lo;
	} else {
		bucket = sample < h->num_buckets ? sample : h->num_buckets;
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
	old_maximum = atomic64_read(&h->maximum);

	while (old_maximum < sample) {
		uint64_t read_value =
			atomic64_cmpxchg(&h->maximum, old_maximum, sample);
		if (read_value == old_maximum) {
			break;
		}
		old_maximum = read_value;
	}

	old_minimum = atomic64_read(&h->minimum);

	while (old_minimum > sample) {
		uint64_t read_value =
			atomic64_cmpxchg(&h->minimum, old_minimum, sample);
		if (read_value == old_minimum) {
			break;
		}
		old_minimum = read_value;
	}
}

/***********************************************************************/
void free_histogram(struct histogram *histogram)
{
	if (histogram != NULL) {
		kobject_put(&histogram->kobj);
	}
}
