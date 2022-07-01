/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef EVENT_COUNT_H
#define EVENT_COUNT_H

#include "time-utils.h"
#include "type-defs.h"

struct event_count;

typedef unsigned int event_token_t;

int __must_check make_event_count(struct event_count **count_ptr);

void free_event_count(struct event_count *count);

void event_count_broadcast(struct event_count *count);

event_token_t __must_check event_count_prepare(struct event_count *count);

void event_count_cancel(struct event_count *count, event_token_t token);

bool event_count_wait(struct event_count *count,
		      event_token_t token,
		      const ktime_t *timeout);

#endif /* EVENT_COUNT_H */
