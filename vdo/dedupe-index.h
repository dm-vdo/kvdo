/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef DEDUPE_INDEX_H
#define DEDUPE_INDEX_H

#include <linux/kobject.h>

#include "uds.h"

#include "kernel-types.h"
#include "statistics.h"
#include "types.h"

int __must_check
vdo_make_dedupe_index(struct vdo *vdo, struct dedupe_index **index_ptr);

void vdo_dump_dedupe_index(struct dedupe_index *index);

void vdo_free_dedupe_index(struct dedupe_index *index);

const char *vdo_get_dedupe_index_state_name(struct dedupe_index *index);

uint64_t vdo_get_dedupe_index_timeout_count(struct dedupe_index *index);

void vdo_get_dedupe_index_statistics(struct dedupe_index *index,
				     struct index_statistics *stats);

int vdo_message_dedupe_index(struct dedupe_index *index, const char *name);

void vdo_query_index(struct data_vio *data_vio,
		     enum uds_request_type operation);

int vdo_add_dedupe_index_sysfs(struct dedupe_index *index,
			       struct kobject *parent);

void vdo_start_dedupe_index(struct dedupe_index *index, bool create_flag);

void vdo_suspend_dedupe_index(struct dedupe_index *index, bool save_flag);

void vdo_resume_dedupe_index(struct dedupe_index *index,
			     struct device_config *config);

void vdo_finish_dedupe_index(struct dedupe_index *index);

/*
 * Interval (in milliseconds) from submission until switching to fast path and
 * skipping UDS.
 */
extern unsigned int vdo_dedupe_index_timeout_interval;

/*
 * Minimum time interval (in milliseconds) between timer invocations to
 * check for requests waiting for UDS that should now time out.
 */
extern unsigned int vdo_dedupe_index_min_timer_interval;

void vdo_set_dedupe_index_timeout_interval(unsigned int value);
void vdo_set_dedupe_index_min_timer_interval(unsigned int value);

bool data_vio_may_query_index(struct data_vio *data_vio);

#endif /* DEDUPE_INDEX_H */
