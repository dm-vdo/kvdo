// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "index-session.h"

#include "index.h"
#include "index-layout.h"
#include "logger.h"
#include "memory-alloc.h"
#include "request-queue.h"
#include "time-utils.h"

/*
 * The index session mediates all interactions with a UDS index. Once the
 * session is created, it can be used to open, close, suspend, or recreate an
 * index. The session contains a lock (the request_mutex) which ensures that
 * only one thread can change the state of its index at a time. The state field
 * indicates the current state of the index through a set of descriptive
 * flags. The request_mutex must be notified whenever a non-transient state
 * flag is cleared. The request_mutex is also used to count the number of
 * requests currently in progress so that they can be drained when suspending
 * or closing the index.
 *
 * If the index session is suspended shortly after opening an index, it may
 * have to suspend during a rebuild. Depending on the size of the index, a
 * rebuild may take a significant amount of time, so UDS allows the rebuild to
 * be paused in order to suspend the session in a timely manner. When the index
 * session is resumed, the rebuild can continue from where it left off. If the
 * index session is shut down with a suspended rebuild, the rebuild progress is
 * abandoned and the rebuild will start from the beginning the next time the
 * index is loaded. The mutex and status fields in the index_load_context are
 * used to record the state of any interrupted rebuild.
 *
 * If any deduplication request fails due to an internal error, the index is
 * marked disabled. It will not accept any further requests and can only be
 * closed. Closing the index will clear the disabled flag, and the index can
 * then be reopened and recovered.
 */

enum index_session_flag_bit {
	IS_FLAG_BIT_START = 8,
	/* The session has started loading an index but not completed it. */
	IS_FLAG_BIT_LOADING = IS_FLAG_BIT_START,
	/* The session has loaded an index, which can handle requests. */
	IS_FLAG_BIT_LOADED,
	/* The session's index has been permanently disabled. */
	IS_FLAG_BIT_DISABLED,
	/* The session's index is suspended. */
	IS_FLAG_BIT_SUSPENDED,
	/* The session is handling some index state change. */
	IS_FLAG_BIT_WAITING,
	/* The session's index is closing and draining requests. */
	IS_FLAG_BIT_CLOSING,
	/* The session is being destroyed and is draining requests. */
	IS_FLAG_BIT_DESTROYING,
};

enum index_session_flag {
	IS_FLAG_LOADED = (1 << IS_FLAG_BIT_LOADED),
	IS_FLAG_LOADING = (1 << IS_FLAG_BIT_LOADING),
	IS_FLAG_DISABLED = (1 << IS_FLAG_BIT_DISABLED),
	IS_FLAG_SUSPENDED = (1 << IS_FLAG_BIT_SUSPENDED),
	IS_FLAG_WAITING = (1 << IS_FLAG_BIT_WAITING),
	IS_FLAG_CLOSING = (1 << IS_FLAG_BIT_CLOSING),
	IS_FLAG_DESTROYING = (1 << IS_FLAG_BIT_DESTROYING),
};

/*
 * Acquire a reference to the index session for an asynchronous index request.
 * The reference must eventually be released with a corresponding call to
 * release_index_session().
 **/
int get_index_session(struct uds_index_session *index_session)
{
	unsigned int state;
	int result = UDS_SUCCESS;

	uds_lock_mutex(&index_session->request_mutex);
	index_session->request_count++;
	state = index_session->state;
	uds_unlock_mutex(&index_session->request_mutex);

	if (state == IS_FLAG_LOADED) {
		return UDS_SUCCESS;
	} else if (state & IS_FLAG_DISABLED) {
		result = UDS_DISABLED;
	} else if ((state & IS_FLAG_LOADING) ||
		   (state & IS_FLAG_SUSPENDED) ||
		   (state & IS_FLAG_WAITING)) {
		result = -EBUSY;
	} else {
		result = UDS_NO_INDEX;
	}

	release_index_session(index_session);
	return result;
}

/* Release a reference to an index session. */
void release_index_session(struct uds_index_session *index_session)
{
	uds_lock_mutex(&index_session->request_mutex);
	if (--index_session->request_count == 0) {
		uds_broadcast_cond(&index_session->request_cond);
	}
	uds_unlock_mutex(&index_session->request_mutex);
}

void disable_index_session(struct uds_index_session *index_session)
{
	uds_lock_mutex(&index_session->request_mutex);
	index_session->state |= IS_FLAG_DISABLED;
	uds_unlock_mutex(&index_session->request_mutex);
}

static void handle_callbacks(struct uds_request *request)
{
	struct uds_index_session *index_session = request->session;

	if (request->status == UDS_SUCCESS) {
		update_request_context_stats(request);
	}

	request->callback(request);
	release_index_session(index_session);
}

static int __must_check
make_empty_index_session(struct uds_index_session **index_session_ptr)
{
	int result;
	struct uds_index_session *session;

	result = UDS_ALLOCATE(1, struct uds_index_session, __func__, &session);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = uds_init_mutex(&session->request_mutex);
	if (result != UDS_SUCCESS) {
		UDS_FREE(session);
		return result;
	}

	result = uds_init_cond(&session->request_cond);
	if (result != UDS_SUCCESS) {
		uds_destroy_mutex(&session->request_mutex);
		UDS_FREE(session);
		return result;
	}

	result = uds_init_mutex(&session->load_context.mutex);
	if (result != UDS_SUCCESS) {
		uds_destroy_cond(&session->request_cond);
		uds_destroy_mutex(&session->request_mutex);
		UDS_FREE(session);
		return result;
	}

	result = uds_init_cond(&session->load_context.cond);
	if (result != UDS_SUCCESS) {
		uds_destroy_mutex(&session->load_context.mutex);
		uds_destroy_cond(&session->request_cond);
		uds_destroy_mutex(&session->request_mutex);
		UDS_FREE(session);
		return result;
	}

	result = make_uds_request_queue("callbackW",
					&handle_callbacks,
					&session->callback_queue);
	if (result != UDS_SUCCESS) {
		uds_destroy_cond(&session->load_context.cond);
		uds_destroy_mutex(&session->load_context.mutex);
		uds_destroy_cond(&session->request_cond);
		uds_destroy_mutex(&session->request_mutex);
		UDS_FREE(session);
		return result;
	}

	*index_session_ptr = session;
	return UDS_SUCCESS;
}

int uds_create_index_session(struct uds_index_session **session)
{
	if (session == NULL) {
		uds_log_error("missing session pointer");
		return -EINVAL;
	}

	return uds_map_to_system_error(make_empty_index_session(session));
}

static int __must_check
start_loading_index_session(struct uds_index_session *index_session)
{
	int result;

	uds_lock_mutex(&index_session->request_mutex);
	if (index_session->state & IS_FLAG_SUSPENDED) {
		uds_log_info("Index session is suspended");
		result = -EBUSY;
	} else if (index_session->state != 0) {
		uds_log_info("Index is already loaded");
		result = -EBUSY;
	} else {
		index_session->state |= IS_FLAG_LOADING;
		result = UDS_SUCCESS;
	}
	uds_unlock_mutex(&index_session->request_mutex);
	return result;
}

static void
finish_loading_index_session(struct uds_index_session *index_session,
			     int result)
{
	uds_lock_mutex(&index_session->request_mutex);
	index_session->state &= ~IS_FLAG_LOADING;
	if (result == UDS_SUCCESS) {
		index_session->state |= IS_FLAG_LOADED;
	}

	uds_broadcast_cond(&index_session->request_cond);
	uds_unlock_mutex(&index_session->request_mutex);
}

static int initialize_index_session(struct uds_index_session *index_session,
				    enum uds_open_index_type open_type)
{
	int result;
	struct configuration *config;

	result = make_configuration(&index_session->parameters, &config);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "Failed to allocate config");
		return result;
	}

	memset(&index_session->stats, 0, sizeof(index_session->stats));
	result = make_index(config,
			    open_type,
			    &index_session->load_context,
			    enter_callback_stage,
			    &index_session->index);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "Failed to make index");
	} else {
		log_uds_configuration(config);
	}

	free_configuration(config);
	return result;
}

static const char *get_open_type_string(enum uds_open_index_type open_type)
{
	switch (open_type) {
	case UDS_CREATE:
		return "creating index";
	case UDS_LOAD:
		return "loading or rebuilding index";
	case UDS_NO_REBUILD:
		return "loading index";
	default:
		return "unknown open method";
	}
}

/*
 * Open an index under the given session. This operation will fail if the
 * index session is suspended, or if there is already an open index.
 */
int uds_open_index(enum uds_open_index_type open_type,
		   const struct uds_parameters *parameters,
		   struct uds_index_session *session)
{
	int result;

	if (parameters == NULL) {
		uds_log_error("missing required parameters");
		return -EINVAL;
	}
	if (parameters->name == NULL) {
		uds_log_error("missing required index name");
		return -EINVAL;
	}
	if (session == NULL) {
		uds_log_error("missing required session pointer");
		return -EINVAL;
	}

	result = start_loading_index_session(session);
	if (result != UDS_SUCCESS) {
		return uds_map_to_system_error(result);
	}

	if ((session->parameters.name == NULL) ||
	    (strcmp(parameters->name, session->parameters.name) != 0)) {
		char *new_name;

		result = uds_duplicate_string(parameters->name,
					      "device name",
					      &new_name);
		if (result != UDS_SUCCESS) {
			finish_loading_index_session(session, result);
			return uds_map_to_system_error(result);
		}

		uds_free_const(session->parameters.name);
		session->parameters = *parameters;
		session->parameters.name = new_name;
	} else {
		const char *old_name = session->parameters.name;

		session->parameters = *parameters;
		session->parameters.name = old_name;
	}

	uds_log_notice("%s: %s",
		       get_open_type_string(open_type),
		       parameters->name);
	result = initialize_index_session(session, open_type);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result,
				       "Failed %s",
				       get_open_type_string(open_type));
	}

	finish_loading_index_session(session, result);
	return uds_map_to_system_error(result);
}

static void
wait_for_no_requests_in_progress(struct uds_index_session *index_session)
{
	uds_lock_mutex(&index_session->request_mutex);
	while (index_session->request_count > 0) {
		uds_wait_cond(&index_session->request_cond,
			      &index_session->request_mutex);
	}
	uds_unlock_mutex(&index_session->request_mutex);
}

static int __must_check uds_save_index(struct uds_index_session *index_session)
{
	wait_for_no_requests_in_progress(index_session);
	return save_index(index_session->index);
}

static void suspend_rebuild(struct uds_index_session *session)
{
	uds_lock_mutex(&session->load_context.mutex);
	switch (session->load_context.status) {
	case INDEX_OPENING:
		session->load_context.status = INDEX_SUSPENDING;

		/* Wait until the index indicates that it is not replaying. */
		while ((session->load_context.status != INDEX_SUSPENDED) &&
		       (session->load_context.status != INDEX_READY)) {
			uds_wait_cond(&session->load_context.cond,
				      &session->load_context.mutex);
		}
		break;

	case INDEX_READY:
		/* Index load does not need to be suspended. */
		break;

	case INDEX_SUSPENDED:
	case INDEX_SUSPENDING:
	case INDEX_FREEING:
	default:
		/* These cases should not happen. */
		ASSERT_LOG_ONLY(false,
				"Bad load context state %u",
				session->load_context.status);
		break;
	}
	uds_unlock_mutex(&session->load_context.mutex);
}

/*
 * Suspend index operation, draining all current index requests and
 * preventing new index requests from starting. Optionally saves all index
 * data before returning.
 */
int uds_suspend_index_session(struct uds_index_session *session, bool save)
{
	int result = UDS_SUCCESS;
	bool no_work = false;
	bool rebuilding = false;

	/* Wait for any current index state change to complete. */
	uds_lock_mutex(&session->request_mutex);
	while (session->state & IS_FLAG_CLOSING) {
		uds_wait_cond(&session->request_cond, &session->request_mutex);
	}

	if ((session->state & IS_FLAG_WAITING) ||
	    (session->state & IS_FLAG_DESTROYING)) {
		no_work = true;
		uds_log_info("Index session is already changing state");
		result = -EBUSY;
	} else if (session->state & IS_FLAG_SUSPENDED) {
		no_work = true;
	} else if (session->state & IS_FLAG_LOADING) {
		session->state |= IS_FLAG_WAITING;
		rebuilding = true;
	} else if (session->state & IS_FLAG_LOADED) {
		session->state |= IS_FLAG_WAITING;
	} else {
		no_work = true;
		session->state |= IS_FLAG_SUSPENDED;
		uds_broadcast_cond(&session->request_cond);
	}
	uds_unlock_mutex(&session->request_mutex);

	if (no_work) {
		return uds_map_to_system_error(result);
	}

	if (rebuilding) {
		suspend_rebuild(session);
	} else if (save) {
		result = uds_save_index(session);
	} else {
		result = uds_flush_index_session(session);
	}

	uds_lock_mutex(&session->request_mutex);
	session->state &= ~IS_FLAG_WAITING;
	session->state |= IS_FLAG_SUSPENDED;
	uds_broadcast_cond(&session->request_cond);
	uds_unlock_mutex(&session->request_mutex);
	return uds_map_to_system_error(result);
}

static int replace_device(struct uds_index_session *session,
			  const char *name)
{
	int result;
	char *new_name;

	result = uds_duplicate_string(name, "device name", &new_name);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = replace_index_storage(session->index, name);
	if (result != UDS_SUCCESS) {
		UDS_FREE(new_name);
		return result;
	}

	uds_free_const(session->parameters.name);
	session->parameters.name = new_name;
	return UDS_SUCCESS;
}

/*
 * Resume index operation after being suspended. If the index is suspended
 * and the supplied name is different from the current backing store, the
 * index will start using the new backing store.
 */
int uds_resume_index_session(struct uds_index_session *session,
			     const char *name)
{
	int result = UDS_SUCCESS;
	bool no_work = false;
	bool resume_replay = false;

	uds_lock_mutex(&session->request_mutex);
	if (session->state & IS_FLAG_WAITING) {
		uds_log_info("Index session is already changing state");
		no_work = true;
		result = -EBUSY;
	} else if (!(session->state & IS_FLAG_SUSPENDED)) {
		/* If not suspended, just succeed. */
		no_work = true;
		result = UDS_SUCCESS;
	} else {
		session->state |= IS_FLAG_WAITING;
		if (session->state & IS_FLAG_LOADING) {
			resume_replay = true;
		}
	}
	uds_unlock_mutex(&session->request_mutex);

	if (no_work) {
		return result;
	}

	if ((name != NULL) && (session->index != NULL) &&
	    (strcmp(name, session->parameters.name) != 0)) {
		result = replace_device(session, name);
		if (result != UDS_SUCCESS) {
			uds_lock_mutex(&session->request_mutex);
			session->state &= ~IS_FLAG_WAITING;
			uds_broadcast_cond(&session->request_cond);
			uds_unlock_mutex(&session->request_mutex);
			return uds_map_to_system_error(result);
		}
	}

	if (resume_replay) {
		uds_lock_mutex(&session->load_context.mutex);
		switch (session->load_context.status) {
		case INDEX_SUSPENDED:
			session->load_context.status = INDEX_OPENING;
			/* Notify the index to start replaying again. */
			uds_broadcast_cond(&session->load_context.cond);
			break;

		case INDEX_READY:
			/* There is no index rebuild to resume. */
			break;

		case INDEX_OPENING:
		case INDEX_SUSPENDING:
		case INDEX_FREEING:
		default:
			/* These cases should not happen; do nothing. */
			ASSERT_LOG_ONLY(false,
					"Bad load context state %u",
					session->load_context.status);
			break;
		}
		uds_unlock_mutex(&session->load_context.mutex);
	}

	uds_lock_mutex(&session->request_mutex);
	session->state &= ~IS_FLAG_WAITING;
	session->state &= ~IS_FLAG_SUSPENDED;
	uds_broadcast_cond(&session->request_cond);
	uds_unlock_mutex(&session->request_mutex);
	return UDS_SUCCESS;
}

static int save_and_free_index(struct uds_index_session *index_session)
{
	int result = UDS_SUCCESS;
	bool suspended;
	struct uds_index *index = index_session->index;

	if (index == NULL) {
		return UDS_SUCCESS;
	}

	uds_lock_mutex(&index_session->request_mutex);
	suspended = (index_session->state & IS_FLAG_SUSPENDED);
	uds_unlock_mutex(&index_session->request_mutex);

	if (!suspended) {
		result = save_index(index);
		if (result != UDS_SUCCESS) {
			uds_log_warning_strerror(result,
						 "ignoring error from save_index");
		}
	}
	free_index(index);
	index_session->index = NULL;

	/*
	 * Reset all index state that happens to be in the index
	 * session, so it doesn't affect any future index.
	 */
	uds_lock_mutex(&index_session->load_context.mutex);
	index_session->load_context.status = INDEX_OPENING;
	uds_unlock_mutex(&index_session->load_context.mutex);

	uds_lock_mutex(&index_session->request_mutex);
	/* Only the suspend bit will remain relevant. */
	index_session->state &= IS_FLAG_SUSPENDED;
	uds_unlock_mutex(&index_session->request_mutex);

	return result;
}

/* Save and close the current index. */
int uds_close_index(struct uds_index_session *index_session)
{
	int result = UDS_SUCCESS;

	/* Wait for any current index state change to complete. */
	uds_lock_mutex(&index_session->request_mutex);
	while ((index_session->state & IS_FLAG_WAITING) ||
	       (index_session->state & IS_FLAG_CLOSING)) {
		uds_wait_cond(&index_session->request_cond,
			      &index_session->request_mutex);
	}

	if (index_session->state & IS_FLAG_SUSPENDED) {
		uds_log_info("Index session is suspended");
		result = -EBUSY;
	} else if ((index_session->state & IS_FLAG_DESTROYING) ||
		   !(index_session->state & IS_FLAG_LOADED)) {
		/*
		 * The index doesn't exist, hasn't finished loading, or is
		 * being destroyed.
		 */
		result = UDS_NO_INDEX;
	} else {
		index_session->state |= IS_FLAG_CLOSING;
	}
	uds_unlock_mutex(&index_session->request_mutex);
	if (result != UDS_SUCCESS) {
		return uds_map_to_system_error(result);
	}

	uds_log_debug("Closing index");
	wait_for_no_requests_in_progress(index_session);
	result = save_and_free_index(index_session);
	uds_log_debug("Closed index");

	uds_lock_mutex(&index_session->request_mutex);
	index_session->state &= ~IS_FLAG_CLOSING;
	uds_broadcast_cond(&index_session->request_cond);
	uds_unlock_mutex(&index_session->request_mutex);
	return uds_map_to_system_error(result);
}

/* This will save and close an open index before destroying the session. */
int uds_destroy_index_session(struct uds_index_session *index_session)
{
	int result;
	bool load_pending = false;

	uds_log_debug("Destroying index session");

	/* Wait for any current index state change to complete. */
	uds_lock_mutex(&index_session->request_mutex);
	while ((index_session->state & IS_FLAG_WAITING) ||
	       (index_session->state & IS_FLAG_CLOSING)) {
		uds_wait_cond(&index_session->request_cond,
			      &index_session->request_mutex);
	}

	if (index_session->state & IS_FLAG_DESTROYING) {
		uds_unlock_mutex(&index_session->request_mutex);
		uds_log_info("Index session is already closing");
		return -EBUSY;
	}

	index_session->state |= IS_FLAG_DESTROYING;
	load_pending = ((index_session->state & IS_FLAG_LOADING) &&
			(index_session->state & IS_FLAG_SUSPENDED));
	uds_unlock_mutex(&index_session->request_mutex);

	if (load_pending) {
		/* Tell the index to terminate the rebuild. */
		uds_lock_mutex(&index_session->load_context.mutex);
		if (index_session->load_context.status == INDEX_SUSPENDED) {
			index_session->load_context.status = INDEX_FREEING;
			uds_broadcast_cond(&index_session->load_context.cond);
		}
		uds_unlock_mutex(&index_session->load_context.mutex);

		/* Wait until the load exits before proceeding. */
		uds_lock_mutex(&index_session->request_mutex);
		while (index_session->state & IS_FLAG_LOADING) {
			uds_wait_cond(&index_session->request_cond,
				      &index_session->request_mutex);
		}
		uds_unlock_mutex(&index_session->request_mutex);
	}

	wait_for_no_requests_in_progress(index_session);
	result = save_and_free_index(index_session);
	uds_free_const(index_session->parameters.name);
	uds_request_queue_finish(index_session->callback_queue);
	index_session->callback_queue = NULL;
	uds_destroy_cond(&index_session->load_context.cond);
	uds_destroy_mutex(&index_session->load_context.mutex);
	uds_destroy_cond(&index_session->request_cond);
	uds_destroy_mutex(&index_session->request_mutex);
	uds_log_debug("Destroyed index session");
	UDS_FREE(index_session);
	return uds_map_to_system_error(result);
}

/* Wait until all callbacks for index operations are complete. */
int uds_flush_index_session(struct uds_index_session *index_session)
{
	wait_for_no_requests_in_progress(index_session);
	wait_for_idle_index(index_session->index);
	return UDS_SUCCESS;
}

/*
 * Return the most recent parameters used to open an index. The caller is
 * responsible for freeing the returned structure.
 */
int uds_get_index_parameters(struct uds_index_session *index_session,
			     struct uds_parameters **parameters)
{
	int result;
	const char *name = index_session->parameters.name;

	if (parameters == NULL) {
		uds_log_error("received a NULL parameters pointer");
		return -EINVAL;
	}

	if (name != NULL) {
		char *name_copy = NULL;
		size_t name_length = strlen(name) + 1;
		struct uds_parameters *copy;

		result = UDS_ALLOCATE_EXTENDED(struct uds_parameters,
					       name_length,
					       char,
					       __func__,
					       &copy);
		if (result != UDS_SUCCESS) {
			return uds_map_to_system_error(result);
		}

		*copy = index_session->parameters;
		name_copy = (char *) copy + sizeof(struct uds_parameters);
		memcpy(name_copy, name, name_length);
		copy->name = name_copy;
		*parameters = copy;
		return UDS_SUCCESS;
	}

	result = UDS_ALLOCATE(1, struct uds_parameters, __func__, parameters);
	if (result == UDS_SUCCESS) {
		**parameters = index_session->parameters;
	}

	return uds_map_to_system_error(result);
}

/* Statistics collection is intended to be thread-safe. */
static void collect_stats(const struct uds_index_session *index_session,
			  struct uds_index_stats *stats)
{
	const struct session_stats *session_stats = &index_session->stats;

	stats->current_time =
		ktime_to_seconds(current_time_ns(CLOCK_REALTIME));
	stats->posts_found = READ_ONCE(session_stats->posts_found);
	stats->in_memory_posts_found =
		READ_ONCE(session_stats->posts_found_open_chapter);
	stats->dense_posts_found = READ_ONCE(session_stats->posts_found_dense);
	stats->sparse_posts_found =
		READ_ONCE(session_stats->posts_found_sparse);
	stats->posts_not_found = READ_ONCE(session_stats->posts_not_found);
	stats->updates_found = READ_ONCE(session_stats->updates_found);
	stats->updates_not_found = READ_ONCE(session_stats->updates_not_found);
	stats->deletions_found = READ_ONCE(session_stats->deletions_found);
	stats->deletions_not_found =
		READ_ONCE(session_stats->deletions_not_found);
	stats->queries_found = READ_ONCE(session_stats->queries_found);
	stats->queries_not_found = READ_ONCE(session_stats->queries_not_found);
	stats->requests = READ_ONCE(session_stats->requests);
}

int uds_get_index_stats(struct uds_index_session *index_session,
			struct uds_index_stats *stats)
{
	if (stats == NULL) {
		uds_log_error("received a NULL index stats pointer");
		return -EINVAL;
	}

	collect_stats(index_session, stats);
	if (index_session->index != NULL) {
		get_index_stats(index_session->index, stats);
	} else {
		stats->entries_indexed = 0;
		stats->memory_used = 0;
		stats->collisions = 0;
		stats->entries_discarded = 0;
	}

	return UDS_SUCCESS;
}
