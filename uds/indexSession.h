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
 * $Id: //eng/uds-releases/lisa/src/uds/indexSession.h#5 $
 */

#ifndef INDEX_SESSION_H
#define INDEX_SESSION_H

#include <linux/atomic.h>

#include "config.h"
#include "cpu.h"
#include "uds-threads.h"
#include "uds.h"

/**
 * The bit position of flags used to indicate index session states.
 **/
enum index_session_flag_bit {
	IS_FLAG_BIT_START = 8,
	/** Flag indicating that the session is loading */
	IS_FLAG_BIT_LOADING = IS_FLAG_BIT_START,
	/** Flag indicating that that the session has been loaded */
	IS_FLAG_BIT_LOADED,
	/** Flag indicating that the session is disabled permanently */
	IS_FLAG_BIT_DISABLED,
	/** Flag indicating that the session is suspended */
	IS_FLAG_BIT_SUSPENDED,
	/** Flag indicating that the session is waiting for an index state
	   change */
	IS_FLAG_BIT_WAITING,
	/** Flag indicating that that the session is closing */
	IS_FLAG_BIT_CLOSING,
	/** Flag indicating that that the session is being destroyed */
	IS_FLAG_BIT_DESTROYING,
};

/**
 * The index session state flags.
 **/
enum index_session_flag {
	IS_FLAG_LOADED = (1 << IS_FLAG_BIT_LOADED),
	IS_FLAG_LOADING = (1 << IS_FLAG_BIT_LOADING),
	IS_FLAG_DISABLED = (1 << IS_FLAG_BIT_DISABLED),
	IS_FLAG_SUSPENDED = (1 << IS_FLAG_BIT_SUSPENDED),
	IS_FLAG_WAITING = (1 << IS_FLAG_BIT_WAITING),
	IS_FLAG_CLOSING = (1 << IS_FLAG_BIT_CLOSING),
	IS_FLAG_DESTROYING = (1 << IS_FLAG_BIT_DESTROYING),
};

struct __attribute__((aligned(CACHE_LINE_BYTES))) session_stats {
	uint64_t posts_found;              /* Posts that found an entry */
	uint64_t posts_found_open_chapter; /* Posts found in the open
					      chapter */
	uint64_t posts_found_dense;        /* Posts found in the dense index */
	uint64_t posts_found_sparse;       /* Posts found in the sparse
					      index */
	uint64_t posts_not_found;          /* Posts that did not find an
					      entry */
	uint64_t updates_found;            /* Updates that found an entry */
	uint64_t updates_not_found;        /* Updates that did not find an
					      entry */
	uint64_t deletions_found;          /* Deletes that found an entry */
	uint64_t deletions_not_found;      /* Deletes that did not find an
					      entry */
	uint64_t queries_found;            /* Queries that found an entry */
	uint64_t queries_not_found;        /* Queries that did not find an
					      entry */
	uint64_t requests;                 /* Total number of requests */
};

/**
 * States used in the index load context, reflecting the state of the index.
 **/
enum index_suspend_status {
	/** The index has not been loaded or rebuilt completely */
	INDEX_OPENING = 0,
	/** The index is able to handle requests */
	INDEX_READY,
	/** The index has a pending request to suspend */
	INDEX_SUSPENDING,
	/** The index is suspended in the midst of a rebuild */
	INDEX_SUSPENDED,
	/** The index is being shut down while suspended */
	INDEX_FREEING,
};

/**
 * The cond_var here must be notified when the status changes to
 * INDEX_SUSPENDED, in order to wake up the waiting uds_suspend_index_session()
 * call. It must also be notified when the status changes away from
 * INDEX_SUSPENDED, to resume rebuild the index from check_for_suspend() in the
 * index.
 **/
struct index_load_context {
	struct mutex mutex;
	struct cond_var cond;
	enum index_suspend_status status; // Covered by
					  // index_load_context.mutex.
};

/**
 * The request cond_var here must be notified when IS_FLAG_WAITING is cleared,
 * in case uds_close_index() or uds_destroy_index_session() is waiting on that
 * flag. It must also be notified when IS_FLAG_CLOSING is cleared, in case
 * uds_suspend_index_session(), uds_close_index() or
 * uds_destroy_index_session() is waiting on that flag. Finally, it must also
 * be notified when IS_FLAG_LOADING is cleared, to inform
 * uds_destroy_index_session() that the index session can be safely freed.
 **/
struct uds_index_session {
	unsigned int state; // Covered by request_mutex.
	struct uds_index *index;
	struct uds_request_queue *callback_queue;
	struct uds_parameters params;
	struct index_load_context load_context;
	// Asynchronous request synchronization
	struct mutex request_mutex;
	struct cond_var request_cond;
	int request_count;
	// Request statistics, all owned by the callback thread
	struct session_stats stats;
};

/**
 * Disable an index session due to an error.
 *
 * @param index_session  the session to be disabled
 **/
void disable_index_session(struct uds_index_session *index_session);

/**
 * Acquire the index session for an asynchronous index request.
 *
 * The pointer must eventually be released with a corresponding call to
 * release_index_session().
 *
 * @param index_session  The index session
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_index_session(struct uds_index_session *index_session);

/**
 * Release a pointer to an index session.
 *
 * @param index_session  The session to release
 **/
void release_index_session(struct uds_index_session *index_session);

#endif /* INDEX_SESSION_H */
