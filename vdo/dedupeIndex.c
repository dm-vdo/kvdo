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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/kernel/dedupeIndex.c#1 $
 */

#include "dedupeIndex.h"

#include <asm/unaligned.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <linux/workqueue.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"
#include "stringUtils.h"
#include "uds.h"

struct uds_attribute {
	struct attribute attr;
	const char *(*show_string)(struct dedupe_index *);
};

enum { UDS_Q_ACTION };

// These are the values in the atomic dedupe_context.request_state field
enum {
	// The uds_request object is not in use.
	UR_IDLE = 0,
	// The uds_request object is in use, and VDO is waiting for the result.
	UR_BUSY = 1,
	// The uds_request object is in use, but has timed out.
	UR_TIMED_OUT = 2,
};

enum index_state {
	// The UDS index is closed
	IS_CLOSED = 0,
	// The UDS index session is opening or closing
	IS_CHANGING = 1,
	// The UDS index is open.
	IS_OPENED = 2,
};

// Data managing the reporting of UDS timeouts
struct periodic_event_reporter {
	uint64_t last_reported_value;
	atomic64_t value;
	struct ratelimit_state ratelimiter;
	struct work_struct work;
};

struct dedupe_index {
	struct kobject dedupe_directory;
	struct registered_thread allocating_thread;
	char *index_name;
	struct uds_configuration *configuration;
	struct uds_parameters uds_params;
	struct uds_index_session *index_session;
	atomic_t active;
	// for reporting UDS timeouts
	struct periodic_event_reporter timeout_reporter;
	// This spinlock protects the state fields and the starting of dedupe
	// requests.
	spinlock_t state_lock;
	struct vdo_work_item work_item; // protected by state_lock
	struct vdo_work_queue *uds_queue; // protected by state_lock
	unsigned int maximum; // protected by state_lock
	enum index_state index_state; // protected by state_lock
	enum index_state index_target; // protected by state_lock
	bool changing; // protected by state_lock
	bool create_flag; // protected by state_lock
	bool dedupe_flag; // protected by state_lock
	bool deduping; // protected by state_lock
	bool error_flag; // protected by state_lock
	bool suspended; // protected by state_lock
	// This spinlock protects the pending list, the pending flag in each
	// vio, and the timeout list.
	spinlock_t pending_lock;
	struct list_head pending_head; // protected by pending_lock
	struct timer_list pending_timer; // protected by pending_lock
	bool started_timer; // protected by pending_lock
};

// Version 1:  user space UDS index (limited to 32 bytes)
// Version 2:  kernel space UDS index (limited to 16 bytes)
enum {
	UDS_ADVICE_VERSION = 2,
	// version byte + state byte + 64-bit little-endian PBN
	UDS_ADVICE_SIZE = 1 + 1 + sizeof(uint64_t),
};

// We want to ensure that there is only one copy of the following constants.
static const char *CLOSED = "closed";
static const char *CLOSING = "closing";
static const char *ERROR = "error";
static const char *OFFLINE = "offline";
static const char *ONLINE = "online";
static const char *OPENING = "opening";
static const char *SUSPENDED = "suspended";
static const char *UNKNOWN = "unknown";

// These times are in milliseconds, and these are the default values.
unsigned int vdo_dedupe_index_timeout_interval = 5000;
unsigned int vdo_dedupe_index_min_timer_interval = 100;

// These times are in jiffies
static uint64_t vdo_dedupe_index_timeout_jiffies;
static uint64_t vdo_dedupe_index_min_timer_jiffies;

/**********************************************************************/
static const char *index_state_to_string(struct dedupe_index *index,
					 enum index_state state)
{
	if (index->suspended) {
		return SUSPENDED;
	}

	switch (state) {
	case IS_CLOSED:
		// Closed. The error_flag tells if it is because of an error.
		return index->error_flag ? ERROR : CLOSED;
	case IS_CHANGING:
		// The index_target tells if we are opening or closing the
		// index.
		return index->index_target == IS_OPENED ? OPENING : CLOSING;
	case IS_OPENED:
		// Opened. The dedupe_flag tells if we are online or offline.
		return index->dedupe_flag ? ONLINE : OFFLINE;
	default:
		return UNKNOWN;
	}
}

/**
 * Encode VDO duplicate advice into the new_metadata field of a UDS request.
 *
 * @param request  The UDS request to receive the encoding
 * @param advice   The advice to encode
 **/
static void encode_uds_advice(struct uds_request *request,
			      struct data_location advice)
{
	size_t offset = 0;
	struct uds_chunk_data *encoding = &request->new_metadata;

	encoding->data[offset++] = UDS_ADVICE_VERSION;
	encoding->data[offset++] = advice.state;
	put_unaligned_le64(advice.pbn, &encoding->data[offset]);
	offset += sizeof(uint64_t);
	BUG_ON(offset != UDS_ADVICE_SIZE);
}

/**
 * Decode VDO duplicate advice from the old_metadata field of a UDS request.
 *
 * @param request  The UDS request containing the encoding
 * @param advice   The data_location to receive the decoded advice
 *
 * @return <code>true</code> if valid advice was found and decoded
 **/
static bool decode_uds_advice(const struct uds_request *request,
			      struct data_location *advice)
{
	size_t offset = 0;
	const struct uds_chunk_data *encoding = &request->old_metadata;
	byte version;

	if ((request->status != UDS_SUCCESS) || !request->found) {
		return false;
	}

	version = encoding->data[offset++];
	if (version != UDS_ADVICE_VERSION) {
		uds_log_error("invalid UDS advice version code %u", version);
		return false;
	}

	advice->state = encoding->data[offset++];
	advice->pbn = get_unaligned_le64(&encoding->data[offset]);
	offset += sizeof(uint64_t);

	BUG_ON(offset != UDS_ADVICE_SIZE);
	return true;
}

/**
 * Calculate the actual end of a timer, taking into account the absolute start
 * time and the present time.
 *
 * @param start_jiffies  The absolute start time, in jiffies
 *
 * @return the absolute end time for the timer, in jiffies
 **/
static uint64_t get_dedupe_index_timeout(uint64_t start_jiffies)
{
	return max(start_jiffies + vdo_dedupe_index_timeout_jiffies,
		   jiffies + vdo_dedupe_index_min_timer_jiffies);
}

/**********************************************************************/
void set_vdo_dedupe_index_timeout_interval(unsigned int value)
{
	uint64_t alb_jiffies;

	// Arbitrary maximum value is two minutes
	if (value > 120000) {
		value = 120000;
	}
	// Arbitrary minimum value is 2 jiffies
	alb_jiffies = msecs_to_jiffies(value);

	if (alb_jiffies < 2) {
		alb_jiffies = 2;
		value = jiffies_to_msecs(alb_jiffies);
	}
	vdo_dedupe_index_timeout_interval = value;
	vdo_dedupe_index_timeout_jiffies = alb_jiffies;
}

/**********************************************************************/
void set_vdo_dedupe_index_min_timer_interval(unsigned int value)
{
	uint64_t min_jiffies;

	// Arbitrary maximum value is one second
	if (value > 1000) {
		value = 1000;
	}

	// Arbitrary minimum value is 2 jiffies
	min_jiffies = msecs_to_jiffies(value);

	if (min_jiffies < 2) {
		min_jiffies = 2;
		value = jiffies_to_msecs(min_jiffies);
	}

	vdo_dedupe_index_min_timer_interval = value;
	vdo_dedupe_index_min_timer_jiffies = min_jiffies;
}

/**********************************************************************/
static void finish_index_operation(struct uds_request *uds_request)
{
	struct data_vio *data_vio = container_of(uds_request,
						 struct data_vio,
						 dedupe_context.uds_request);
	struct dedupe_context *dedupe_context = &data_vio->dedupe_context;

	if (atomic_cmpxchg(&dedupe_context->request_state,
			   UR_BUSY, UR_IDLE) == UR_BUSY) {
		struct vio *vio = data_vio_as_vio(data_vio);
		struct dedupe_index *index = vio->vdo->dedupe_index;

		spin_lock_bh(&index->pending_lock);
		if (dedupe_context->is_pending) {
			list_del(&dedupe_context->pending_list);
			dedupe_context->is_pending = false;
		}
		spin_unlock_bh(&index->pending_lock);

		dedupe_context->status = uds_request->status;
		if ((uds_request->type == UDS_POST) ||
		    (uds_request->type == UDS_QUERY)) {
			struct data_location advice;

			if (decode_uds_advice(uds_request, &advice)) {
				vdo_set_dedupe_advice(dedupe_context, &advice);
			} else {
				vdo_set_dedupe_advice(dedupe_context, NULL);
			}
		}

		enqueue_data_vio_callback(data_vio);
		atomic_dec(&index->active);
	} else {
		atomic_cmpxchg(&dedupe_context->request_state,
			       UR_TIMED_OUT,
			       UR_IDLE);
	}
}

/**
 * Must be called holding pending_lock
 **/
static void start_expiration_timer(struct dedupe_index *index,
				   unsigned long expiration)
{
	if (!index->started_timer) {
		index->started_timer = true;
		mod_timer(&index->pending_timer, expiration);
	}
}

/**
 * Must be called holding pending_lock
 **/
static void start_expiration_timer_for_vio(struct dedupe_index *index,
					   struct data_vio *data_vio)
{
	struct dedupe_context *context = &data_vio->dedupe_context;
	uint64_t start_time = context->submission_jiffies;
	start_expiration_timer(index, get_dedupe_index_timeout(start_time));
}

/**********************************************************************/
static void start_index_operation(struct vdo_work_item *item)
{
	struct vio *vio = work_item_as_vio(item);
	struct data_vio *data_vio = vio_as_data_vio(vio);
	struct dedupe_index *index = vio->vdo->dedupe_index;
	struct dedupe_context *dedupe_context = &data_vio->dedupe_context;
	struct uds_request *uds_request = &dedupe_context->uds_request;
	int status;

	spin_lock_bh(&index->pending_lock);
	list_add_tail(&dedupe_context->pending_list, &index->pending_head);
	dedupe_context->is_pending = true;
	start_expiration_timer_for_vio(index, data_vio);
	spin_unlock_bh(&index->pending_lock);

	status = uds_start_chunk_operation(uds_request);
	if (status != UDS_SUCCESS) {
		uds_request->status = status;
		finish_index_operation(uds_request);
	}
}

/**********************************************************************/
uint64_t get_vdo_dedupe_index_timeout_count(struct dedupe_index *index)
{
	return atomic64_read(&index->timeout_reporter.value);
}

/**********************************************************************/
static void report_events(struct periodic_event_reporter *reporter,
			  bool ratelimit)
{
	uint64_t new_value = atomic64_read(&reporter->value);
	uint64_t difference = new_value - reporter->last_reported_value;

	if (difference != 0) {
		if (!ratelimit || __ratelimit(&reporter->ratelimiter)) {
			uds_log_debug("UDS index timeout on %llu requests",
				      difference);
			reporter->last_reported_value = new_value;
		} else {
			/**
			 * Turn on a backup timer that will fire after the
			 * current interval. Just in case the last index
			 * request in a while times out; we want to report
			 * the dedupe timeouts in a timely manner in such cases
			 **/
			struct dedupe_index *index =
				container_of(reporter,
					     struct dedupe_index,
					     timeout_reporter);
			spin_lock_bh(&index->pending_lock);
			start_expiration_timer(index,
				jiffies + reporter->ratelimiter.interval + 1);
			spin_unlock_bh(&index->pending_lock);
		}
	}
}

/**********************************************************************/
static void report_events_work(struct work_struct *work)
{
	struct periodic_event_reporter *reporter =
		container_of(work, struct periodic_event_reporter, work);
	report_events(reporter, true);
}

/**********************************************************************/
static void
init_periodic_event_reporter(struct periodic_event_reporter *reporter)
{
	INIT_WORK(&reporter->work, report_events_work);
	ratelimit_default_init(&reporter->ratelimiter);
	// Since we will save up the timeouts that would have been reported
	// but were ratelimited, we don't need to report ratelimiting.
	ratelimit_set_flags(&reporter->ratelimiter, RATELIMIT_MSG_ON_RELEASE);
}

/**
 * Record and eventually report that some dedupe requests reached their
 * expiration time without getting answers, so we timed them out.
 *
 * This is called in a timer context, so it shouldn't do the reporting
 * directly.
 *
 * @param reporter       The periodic event reporter
 * @param timeouts       How many requests were timed out.
 **/
static void report_dedupe_timeouts(struct periodic_event_reporter *reporter,
				   unsigned int timeouts)
{
	atomic64_add(timeouts, &reporter->value);
	// If it's already queued, requeueing it will do nothing.
	schedule_work(&reporter->work);
}

/**********************************************************************/
static void
stop_periodic_event_reporter(struct periodic_event_reporter *reporter)
{
	cancel_work_sync(&reporter->work);
	report_events(reporter, false);
	ratelimit_state_exit(&reporter->ratelimiter);
}

/**********************************************************************/
static void timeout_index_operations(struct timer_list *t)
{
	struct dedupe_index *index = from_timer(index, t, pending_timer);
	LIST_HEAD(expired_head);
	uint64_t timeout_jiffies =
		msecs_to_jiffies(vdo_dedupe_index_timeout_interval);
	unsigned long earliest_submission_allowed = jiffies - timeout_jiffies;
	unsigned int timed_out = 0;

	spin_lock_bh(&index->pending_lock);
	index->started_timer = false;
	while (!list_empty(&index->pending_head)) {
		struct data_vio *data_vio =
			list_first_entry(&index->pending_head,
					 struct data_vio,
					 dedupe_context.pending_list);
		struct dedupe_context *dedupe_context =
			&data_vio->dedupe_context;
		if (earliest_submission_allowed <=
		    dedupe_context->submission_jiffies) {
			start_expiration_timer_for_vio(index, data_vio);
			break;
		}
		list_del(&dedupe_context->pending_list);
		dedupe_context->is_pending = false;
		list_add_tail(&dedupe_context->pending_list, &expired_head);
	}
	spin_unlock_bh(&index->pending_lock);
	while (!list_empty(&expired_head)) {
		struct data_vio *data_vio =
			list_first_entry(&expired_head,
					 struct data_vio,
					 dedupe_context.pending_list);
		struct dedupe_context *dedupe_context =
			&data_vio->dedupe_context;
		list_del(&dedupe_context->pending_list);
		if (atomic_cmpxchg(&dedupe_context->request_state,
				   UR_BUSY, UR_TIMED_OUT) == UR_BUSY) {
			dedupe_context->status = ETIMEDOUT;
			enqueue_data_vio_callback(data_vio);
			atomic_dec(&index->active);
			timed_out++;
		}
	}
	report_dedupe_timeouts(&index->timeout_reporter, timed_out);
}

/**********************************************************************/
void enqueue_vdo_index_operation(struct data_vio *data_vio,
				 enum uds_request_type operation)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	struct dedupe_context *dedupe_context = &data_vio->dedupe_context;
	struct dedupe_index *index = vio->vdo->dedupe_index;

	dedupe_context->status = UDS_SUCCESS;
	dedupe_context->submission_jiffies = jiffies;
	if (atomic_cmpxchg(&dedupe_context->request_state,
			   UR_IDLE, UR_BUSY) == UR_IDLE) {
		struct uds_request *uds_request =
			&data_vio->dedupe_context.uds_request;

		uds_request->chunk_name = data_vio->chunk_name;
		uds_request->callback = finish_index_operation;
		uds_request->session = index->index_session;
		uds_request->type = operation;
		uds_request->update = true;
		if ((operation == UDS_POST) || (operation == UDS_UPDATE)) {
			encode_uds_advice(uds_request,
					  vdo_get_dedupe_advice(dedupe_context));
		}

		setup_work_item(work_item_from_vio(vio),
				start_index_operation,
				NULL,
				UDS_Q_ACTION);

		spin_lock(&index->state_lock);
		if (index->deduping) {
			unsigned int active;

			enqueue_work_queue(index->uds_queue,
					   work_item_from_vio(vio));

			active = atomic_inc_return(&index->active);
			if (active > index->maximum) {
				index->maximum = active;
			}
			vio = NULL;
		} else {
			atomic_set(&dedupe_context->request_state, UR_IDLE);
		}
		spin_unlock(&index->state_lock);
	} else {
		// A previous user of the vio had a dedupe timeout
		// and its request is still outstanding.
		atomic64_inc(&vio->vdo->stats.dedupe_context_busy);
	}

	if (vio != NULL) {
		enqueue_data_vio_callback(data_vio);
	}
}

/**********************************************************************/
static void close_index(struct dedupe_index *index)
{
	int result;

	// Change the index state so that get_vdo_dedupe_index_statistics will
	// not try to use the index session we are closing.
	index->index_state = IS_CHANGING;
	// Close the index session, while not holding the state_lock.
	spin_unlock(&index->state_lock);
	result = uds_close_index(index->index_session);

	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result,
				       "Error closing index %s",
				       index->index_name);
	}
	spin_lock(&index->state_lock);
	index->index_state = IS_CLOSED;
	index->error_flag |= result != UDS_SUCCESS;
	// ASSERTION: We leave in IS_CLOSED state.
}

/**********************************************************************/
static void open_index(struct dedupe_index *index)
{
	// ASSERTION: We enter in IS_CLOSED state.
	int result;
	bool create_flag = index->create_flag;

	index->create_flag = false;
	// Change the index state so that the it will be reported to the
	// outside world as "opening".
	index->index_state = IS_CHANGING;
	index->error_flag = false;
	// Open the index session, while not holding the state_lock
	spin_unlock(&index->state_lock);
	result = uds_open_index(create_flag ? UDS_CREATE : UDS_LOAD,
				index->index_name, &index->uds_params,
				index->configuration, index->index_session);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result,
				       "Error opening index %s",
				       index->index_name);
	}
	spin_lock(&index->state_lock);
	if (!create_flag) {
		switch (result) {
		case -ENOENT:
			// Either there is no index, or there is no way we can
			// recover the index. We will be called again and try
			// to create a new index.
			index->index_state = IS_CLOSED;
			index->create_flag = true;
			return;
		default:
			break;
		}
	}
	if (result == UDS_SUCCESS) {
		index->index_state = IS_OPENED;
	} else {
		index->index_state = IS_CLOSED;
		index->index_target = IS_CLOSED;
		index->error_flag = true;
		spin_unlock(&index->state_lock);
		uds_log_info("Setting UDS index target state to error");
		spin_lock(&index->state_lock);
	}
	// ASSERTION: On success, we leave in IS_OPENED state.
	// ASSERTION: On failure, we leave in IS_CLOSED state.
}

/**********************************************************************/
static void change_dedupe_state(struct vdo_work_item *item)
{
	struct dedupe_index *index = container_of(item,
						  struct dedupe_index,
						  work_item);
	spin_lock(&index->state_lock);

	// Loop until the index is in the target state and the create flag is
	// clear.
	while (!index->suspended &&
	       ((index->index_state != index->index_target) ||
		index->create_flag)) {
		if (index->index_state == IS_OPENED) {
			close_index(index);
		} else {
			open_index(index);
		}
	}
	index->changing = false;
	index->deduping =
		index->dedupe_flag && (index->index_state == IS_OPENED);
	spin_unlock(&index->state_lock);
}

/**********************************************************************/
static void launch_dedupe_state_change(struct dedupe_index *index)
{
	// ASSERTION: We enter with the state_lock held.
	if (index->changing || index->suspended) {
		// Either a change is already in progress, or changes are
		// not allowed.
		return;
	}

	if (index->create_flag ||
	    (index->index_state != index->index_target)) {
		index->changing = true;
		index->deduping = false;
		setup_work_item(&index->work_item,
				change_dedupe_state,
				NULL,
				UDS_Q_ACTION);
		enqueue_work_queue(index->uds_queue, &index->work_item);
		return;
	}

	// Online vs. offline changes happen immediately
	index->deduping = (index->dedupe_flag && !index->suspended &&
			   (index->index_state == IS_OPENED));

	// ASSERTION: We exit with the state_lock held.
}

/**********************************************************************/
static void set_target_state(struct dedupe_index *index,
			     enum index_state target,
			     bool change_dedupe,
			     bool dedupe,
			     bool set_create)
{
	const char *old_state, *new_state;

	spin_lock(&index->state_lock);
	old_state = index_state_to_string(index, index->index_target);
	if (change_dedupe) {
		index->dedupe_flag = dedupe;
	}
	if (set_create) {
		index->create_flag = true;
	}
	index->index_target = target;
	launch_dedupe_state_change(index);
	new_state = index_state_to_string(index, index->index_target);
	spin_unlock(&index->state_lock);

	if (old_state != new_state) {
		uds_log_info("Setting UDS index target state to %s",
			     new_state);
	}
}

/**********************************************************************/
void suspend_vdo_dedupe_index(struct dedupe_index *index, bool save_flag)
{
	enum index_state state;

	spin_lock(&index->state_lock);
	index->suspended = true;
	state = index->index_state;
	spin_unlock(&index->state_lock);

	if (state != IS_CLOSED) {
		int result = uds_suspend_index_session(index->index_session,
						       save_flag);
		if (result != UDS_SUCCESS) {
			uds_log_error_strerror(result,
					       "Error suspending dedupe index");
		}
	}
}

/**********************************************************************/
void resume_vdo_dedupe_index(struct dedupe_index *index)
{
	int result = uds_resume_index_session(index->index_session);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "Error resuming dedupe index");
	}

	spin_lock(&index->state_lock);
	index->suspended = false;
	launch_dedupe_state_change(index);
	spin_unlock(&index->state_lock);
}


/**********************************************************************/
void dump_vdo_dedupe_index(struct dedupe_index *index, bool show_queue)
{
	const char *state, *target;

	spin_lock(&index->state_lock);
	state = index_state_to_string(index, index->index_state);
	target = (index->changing ?
			 index_state_to_string(index, index->index_target) :
			 NULL);
	spin_unlock(&index->state_lock);

	uds_log_info("UDS index: state: %s", state);
	if (target != NULL) {
		uds_log_info("UDS index: changing to state: %s", target);
	}
	if (show_queue) {
		dump_work_queue(index->uds_queue);
	}
}

/**********************************************************************/
void finish_vdo_dedupe_index(struct dedupe_index *index)
{
	if (index == NULL) {
		return;
	}

	set_target_state(index, IS_CLOSED, false, false, false);
	uds_destroy_index_session(index->index_session);
	finish_work_queue(index->uds_queue);
}

/**********************************************************************/
void free_vdo_dedupe_index(struct dedupe_index *index)
{
	if (index == NULL) {
		return;
	}

	free_work_queue(UDS_FORGET(index->uds_queue));
	stop_periodic_event_reporter(&index->timeout_reporter);
	spin_lock_bh(&index->pending_lock);
	if (index->started_timer) {
		del_timer_sync(&index->pending_timer);
	}
	spin_unlock_bh(&index->pending_lock);
	kobject_put(&index->dedupe_directory);
}

/**********************************************************************/
const char *get_vdo_dedupe_index_state_name(struct dedupe_index *index)
{
	const char *state;

	spin_lock(&index->state_lock);
	state = index_state_to_string(index, index->index_state);
	spin_unlock(&index->state_lock);

	return state;
}

/**********************************************************************/
void get_vdo_dedupe_index_statistics(struct dedupe_index *index,
				     struct index_statistics *stats)
{
	enum index_state state;

	spin_lock(&index->state_lock);
	state = index->index_state;
	stats->max_dedupe_queries = index->maximum;
	spin_unlock(&index->state_lock);

	stats->curr_dedupe_queries = atomic_read(&index->active);
	if (state == IS_OPENED) {
		struct uds_index_stats index_stats;
		int result = uds_get_index_stats(index->index_session,
						 &index_stats);
		if (result == UDS_SUCCESS) {
			stats->entries_indexed = index_stats.entries_indexed;
			stats->posts_found = index_stats.posts_found;
			stats->posts_not_found = index_stats.posts_not_found;
			stats->queries_found = index_stats.queries_found;
			stats->queries_not_found =
				index_stats.queries_not_found;
			stats->updates_found = index_stats.updates_found;
			stats->updates_not_found =
				index_stats.updates_not_found;
		} else {
			uds_log_error_strerror(result,
					       "Error reading index stats");
		}
	}
}


/**********************************************************************/
int message_vdo_dedupe_index(struct dedupe_index *index, const char *name)
{
	if (strcasecmp(name, "index-close") == 0) {
		set_target_state(index, IS_CLOSED, false, false, false);
		return 0;
	} else if (strcasecmp(name, "index-create") == 0) {
		set_target_state(index, IS_OPENED, false, false, true);
		return 0;
	} else if (strcasecmp(name, "index-disable") == 0) {
		set_target_state(index, IS_OPENED, true, false, false);
		return 0;
	} else if (strcasecmp(name, "index-enable") == 0) {
		set_target_state(index, IS_OPENED, true, true, false);
		return 0;
	}
	return -EINVAL;
}

/**********************************************************************/
void start_vdo_dedupe_index(struct dedupe_index *index, bool create_flag)
{
	set_target_state(index, IS_OPENED, true, true, create_flag);
}

/**********************************************************************/
void stop_vdo_dedupe_index(struct dedupe_index *index)
{
	set_target_state(index, IS_CLOSED, false, false, false);
}

/**********************************************************************/
static void dedupe_kobj_release(struct kobject *directory)
{
	struct dedupe_index *index = container_of(directory,
						  struct dedupe_index,
						  dedupe_directory);
	uds_free_configuration(index->configuration);
	UDS_FREE(index->index_name);
	UDS_FREE(index);
}

/**********************************************************************/
static ssize_t dedupe_status_show(struct kobject *directory,
				  struct attribute *attr,
				  char *buf)
{
	struct uds_attribute *ua =
		container_of(attr, struct uds_attribute, attr);
	struct dedupe_index *index =
		container_of(directory, struct dedupe_index, dedupe_directory);
	if (ua->show_string != NULL) {
		return sprintf(buf, "%s\n", ua->show_string(index));
	} else {
		return -EINVAL;
	}
}

/**********************************************************************/
static ssize_t dedupe_status_store(struct kobject *kobj,
				   struct attribute *attr,
				   const char *buf,
				   size_t length)
{
	return -EINVAL;
}

/**********************************************************************/

static struct sysfs_ops dedupe_sysfs_ops = {
	.show = dedupe_status_show,
	.store = dedupe_status_store,
};

static struct uds_attribute dedupe_status_attribute = {
	.attr = {.name = "status", .mode = 0444, },
	.show_string = get_vdo_dedupe_index_state_name,
};

static struct attribute *dedupe_attributes[] = {
	&dedupe_status_attribute.attr,
	NULL,
};

static struct kobj_type dedupe_directory_type = {
	.release = dedupe_kobj_release,
	.sysfs_ops = &dedupe_sysfs_ops,
	.default_attrs = dedupe_attributes,
};

/**********************************************************************/
static void start_uds_queue(void *ptr)
{
	/*
	 * Allow the UDS dedupe worker thread to do memory allocations. It
	 * will only do allocations during the UDS calls that open or close an
	 * index, but those allocations can safely sleep while reserving a
	 * large amount of memory. We could use an allocations_allowed boolean
	 * (like the base threads do), but it would be an unnecessary
	 * embellishment.
	 */
	struct dedupe_index *index = ptr;
	uds_register_allocating_thread(&index->allocating_thread, NULL);
}

/**********************************************************************/
static void finish_uds_queue(void *ptr __always_unused)
{
	uds_unregister_allocating_thread();
}

/**********************************************************************/
int make_vdo_dedupe_index(struct dedupe_index **index_ptr,
			  struct vdo *vdo,
			  const char* thread_name_prefix)
{
	int result;
	off_t uds_offset;
	struct dedupe_index *index;
	struct index_config *index_config;
	static const struct vdo_work_queue_type uds_queue_type = {
		.start = start_uds_queue,
		.finish = finish_uds_queue,
		.action_table = {
			{ .name = "uds_action",
			  .code = UDS_Q_ACTION,
			  .priority = 0 },
		},
	};
	set_vdo_dedupe_index_timeout_interval(vdo_dedupe_index_timeout_interval);
	set_vdo_dedupe_index_min_timer_interval(vdo_dedupe_index_min_timer_interval);

	result = UDS_ALLOCATE(1, struct dedupe_index, "UDS index data", &index);

	if (result != UDS_SUCCESS) {
		return result;
	}

	uds_offset = ((vdo_get_index_region_start(vdo->geometry) -
		       vdo->geometry.bio_offset) * VDO_BLOCK_SIZE);
	result = uds_alloc_sprintf("index name", &index->index_name,
			       "dev=%s offset=%ld size=%llu",
			       vdo->device_config->parent_device_name,
			       uds_offset,
			       (vdo_get_index_region_size(vdo->geometry) *
				VDO_BLOCK_SIZE));
	if (result != UDS_SUCCESS) {
		uds_log_error("Creating index name failed (%d)", result);
		UDS_FREE(index);
		return result;
	}

	index->uds_params = (struct uds_parameters) UDS_PARAMETERS_INITIALIZER;
	index_config = &vdo->geometry.index_config;
	vdo_index_config_to_uds_parameters(index_config, &index->uds_params);
	result = vdo_index_config_to_uds_configuration(index_config,
						       &index->configuration);
	if (result != VDO_SUCCESS) {
		UDS_FREE(index->index_name);
		UDS_FREE(index);
		return result;
	}
	uds_configuration_set_nonce(index->configuration,
				    (uds_nonce_t) vdo->geometry.nonce);

	result = uds_create_index_session(&index->index_session);
	if (result != UDS_SUCCESS) {
		uds_free_configuration(index->configuration);
		UDS_FREE(index->index_name);
		UDS_FREE(index);
		return result;
	}

	result = make_work_queue(thread_name_prefix,
				 "dedupeQ",
				 &vdo->work_queue_directory,
				 vdo,
				 index,
				 &uds_queue_type,
				 1,
				 NULL,
				 &index->uds_queue);
	if (result != VDO_SUCCESS) {
		uds_log_error("UDS index queue initialization failed (%d)",
			  result);
		uds_destroy_index_session(index->index_session);
		uds_free_configuration(index->configuration);
		UDS_FREE(index->index_name);
		UDS_FREE(index);
		return result;
	}

	kobject_init(&index->dedupe_directory, &dedupe_directory_type);
	result = kobject_add(&index->dedupe_directory,
			     &vdo->vdo_directory,
			     "dedupe");
	if (result != VDO_SUCCESS) {
		free_work_queue(UDS_FORGET(index->uds_queue));
		uds_destroy_index_session(index->index_session);
		uds_free_configuration(index->configuration);
		UDS_FREE(index->index_name);
		UDS_FREE(index);
		return result;
	}

	INIT_LIST_HEAD(&index->pending_head);
	spin_lock_init(&index->pending_lock);
	spin_lock_init(&index->state_lock);
	timer_setup(&index->pending_timer, timeout_index_operations, 0);

	// UDS Timeout Reporter
	init_periodic_event_reporter(&index->timeout_reporter);

	*index_ptr = index;
	return VDO_SUCCESS;
}
