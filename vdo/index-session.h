/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INDEX_SESSION_H
#define INDEX_SESSION_H

#include <linux/atomic.h>

#include "config.h"
#include "cpu.h"
#include "uds-threads.h"
#include "uds.h"

struct __attribute__((aligned(CACHE_LINE_BYTES))) session_stats {
	/* Post requests that found an entry */
	uint64_t posts_found;
	/* Post requests found in the open chapter */
	uint64_t posts_found_open_chapter;
	/* Post requests found in the dense index */
	uint64_t posts_found_dense;
	/* Post requests found in the sparse index */
	uint64_t posts_found_sparse;
	/* Post requests that did not find an entry */
	uint64_t posts_not_found;
	/* Update requests that found an entry */
	uint64_t updates_found;
	/* Update requests that did not find an entry */
	uint64_t updates_not_found;
	/* Delete requests that found an entry */
	uint64_t deletions_found;
	/* Delete requests that did not find an entry */
	uint64_t deletions_not_found;
	/* Query requests that found an entry */
	uint64_t queries_found;
	/* Query requests that did not find an entry */
	uint64_t queries_not_found;
	/* Total number of requests */
	uint64_t requests;
};

enum index_suspend_status {
	/* An index load has started but the index is not ready for use. */
	INDEX_OPENING = 0,
	/* The index is able to handle requests. */
	INDEX_READY,
	/* The index is attempting to suspend a rebuild. */
	INDEX_SUSPENDING,
	/* An index rebuild has been suspended. */
	INDEX_SUSPENDED,
	/* An index rebuild is being stopped in order to shut down. */
	INDEX_FREEING,
};

struct index_load_context {
	struct mutex mutex;
	struct cond_var cond;
	enum index_suspend_status status;
};

struct uds_index_session {
	unsigned int state;
	struct uds_index *index;
	struct uds_request_queue *callback_queue;
	struct uds_parameters parameters;
	struct index_load_context load_context;
	struct mutex request_mutex;
	struct cond_var request_cond;
	int request_count;
	struct session_stats stats;
};

void disable_index_session(struct uds_index_session *index_session);

int __must_check get_index_session(struct uds_index_session *index_session);

void release_index_session(struct uds_index_session *index_session);

#endif /* INDEX_SESSION_H */
