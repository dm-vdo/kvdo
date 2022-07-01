// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "permassert.h"
#include "string-utils.h"
#include "time-utils.h"

#include <linux/delay.h>
#include <linux/ktime.h>


int64_t current_time_us(void)
{
	return current_time_ns(CLOCK_REALTIME) / NSEC_PER_USEC;
}



