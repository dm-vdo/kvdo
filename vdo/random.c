// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "random.h"

#include "permassert.h"

unsigned int random_in_range(unsigned int lo, unsigned int hi)
{
	return lo + random() % (hi - lo + 1);
}

void random_compile_time_assertions(void)
{
	STATIC_ASSERT((((uint64_t) RAND_MAX + 1) & RAND_MAX) == 0);
}

