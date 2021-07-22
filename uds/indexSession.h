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
 * $Id: //eng/uds-releases/krusty/src/uds/indexSession.h#11 $
 */

#ifndef INDEX_SESSION_H
#define INDEX_SESSION_H

#include <linux/atomic.h>

#include "config.h"
#include "cpu.h"
#include "opaqueTypes.h"
#include "threads.h"
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
	struct index_router *router;
	struct uds_request_queue *callback_queue;
	struct uds_configuration user_config;
	struct index_load_context load_context;
	// Asynchronous Request synchronization
	struct mutex request_mutex;
	struct cond_var request_cond;
	int request_count;
	// Request statistics, all owned by the callback thread
	struct session_stats stats;
};

/**
 * Check that the index session is usable.
 *
 * @param index_session  the session to query
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check check_index_session(struct uds_index_session *index_session);

/**
 * Make sure that the index_session is allowed to load an index, and if so, set
 * its state to indicate that the load has started.
 *
 * @param index_session  the session to load with
 *
 * @return UDS_SUCCESS, or an error code if an index already exists.
 **/
int __must_check
start_loading_index_session(struct uds_index_session *index_session);

/**
 * Update the index_session state after attempting to load an index, to
 * indicate that the load has completed, and whether or not it succeeded.
 *
 * @param index_session  the session that was loading
 * @param result        the result of the load operation
 **/
void finish_loading_index_session(struct uds_index_session *index_session,
				  int result);

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

/**
 * Construct a new, empty index session.
 *
 * @param index_session_ptr   The pointer to receive the new session
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
make_empty_index_session(struct uds_index_session **index_session_ptr);

/**
 * Save an index while the session is quiescent.
 *
 * During the call to #uds_save_index, there should be no other call to
 * #uds_save_index and there should be no calls to #uds_start_chunk_operation.
 *
 * @param index_session  The session to save
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_save_index(struct uds_index_session *index_session);

/**
 * Close the index by saving the underlying index.
 *
 * @param index_session  The index session to be shut down and freed
 **/
int save_and_free_index(struct uds_index_session *index_session);

/**
 * Set the checkpoint frequency of the grid.
 *
 * @param session    The index session to be modified.
 * @param frequency  New checkpoint frequency.
 *
 * @return          Either UDS_SUCCESS or an error code.
 *
 **/
int __must_check
uds_set_checkpoint_frequency(struct uds_index_session *session,
			     unsigned int frequency);

#endif /* INDEX_SESSION_H */
