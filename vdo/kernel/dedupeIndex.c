/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dedupeIndex.c#61 $
 */

#include "dedupeIndex.h"

#include <linux/ratelimit.h>
#include <linux/workqueue.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"
#include "numeric.h"
#include "stringUtils.h"
#include "uds.h"

/*****************************************************************************/

struct uds_attribute {
	struct attribute attr;
	const char *(*show_string)(struct dedupe_index *);
};

/*****************************************************************************/

enum { UDS_Q_ACTION };

/*****************************************************************************/

// These are the values in the atomic dedupe_context.request_state field
enum {
	// The uds_request object is not in use.
	UR_IDLE = 0,
	// The uds_request object is in use, and VDO is waiting for the result.
	UR_BUSY = 1,
	// The uds_request object is in use, but has timed out.
	UR_TIMED_OUT = 2,
};

/*****************************************************************************/

typedef enum {
	// The UDS index is closed
	IS_CLOSED = 0,
	// The UDS index session is opening or closing
	IS_CHANGING = 1,
	// The UDS index is open.
	IS_OPENED = 2,
} index_state;

// Data managing the reporting of UDS timeouts
struct periodic_event_reporter {
	uint64_t last_reported_value;
	atomic64_t value;
	struct ratelimit_state ratelimiter;
	struct work_struct work;
	struct kernel_layer *layer;
};

/*****************************************************************************/

struct dedupe_index {
	struct kobject dedupe_object;
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
	struct kvdo_work_item work_item; // protected by state_lock
	struct kvdo_work_queue *uds_queue; // protected by state_lock
	unsigned int maximum; // protected by state_lock
	index_state index_state; // protected by state_lock
	index_state index_target; // protected by state_lock
	bool changing; // protected by state_lock
	bool create_flag; // protected by state_lock
	bool dedupe_flag; // protected by state_lock
	bool deduping; // protected by state_lock
	bool error_flag; // protected by state_lock
	bool suspended; // protected by state_lock
	// This spinlock protects the pending list, the pending flag in each
	// kvio, and the timeout list.
	spinlock_t pending_lock;
	struct list_head pending_head; // protected by pending_lock
	struct timer_list pending_timer; // protected by pending_lock
	bool started_timer; // protected by pending_lock
};

/*****************************************************************************/

// Version 1:  user space UDS index (limited to 32 bytes)
// Version 2:  kernel space UDS index (limited to 16 bytes)
enum { UDS_ADVICE_VERSION = 2,
       // version byte + state byte + 64-bit little-endian PBN
       UDS_ADVICE_SIZE = 1 + 1 + sizeof(uint64_t),
};

/*****************************************************************************/

// We want to ensure that there is only one copy of the following constants.
static const char *CLOSED = "closed";
static const char *CLOSING = "closing";
static const char *ERROR = "error";
static const char *OFFLINE = "offline";
static const char *ONLINE = "online";
static const char *OPENING = "opening";
static const char *SUSPENDED = "suspended";
static const char *UNKNOWN = "unknown";

/*****************************************************************************/

// These times are in milliseconds, and these are the default values.
unsigned int albireo_timeout_interval = 5000;
unsigned int min_albireo_timer_interval = 100;

// These times are in jiffies
static Jiffies albireo_timeout_jiffies;
static Jiffies min_albireo_timer_jiffies;

/*****************************************************************************/
static const char *index_state_to_string(struct dedupe_index *index,
					 index_state state)
{
	if (index->suspended) {
		return SUSPENDED;
	}

	switch (state) {
	case IS_CLOSED:
		// Closed.  The error_flag tells if it is because of an error.
		return index->error_flag ? ERROR : CLOSED;
	case IS_CHANGING:
		// The index_target tells if we are opening or closing the
		// index.
		return index->index_target == IS_OPENED ? OPENING : CLOSING;
	case IS_OPENED:
		// Opened.  The dedupe_flag tells if we are online or offline.
		return index->dedupe_flag ? ONLINE : OFFLINE;
	default:
		return UNKNOWN;
	}
}

/**
 * Encode VDO duplicate advice into the newMetadata field of a UDS request.
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
	encode_uint64_le(encoding->data, &offset, advice.pbn);
	BUG_ON(offset != UDS_ADVICE_SIZE);
}

/**
 * Decode VDO duplicate advice from the oldMetadata field of a UDS request.
 *
 * @param request  The UDS request containing the encoding
 * @param advice   The data_location to receive the decoded advice
 *
 * @return <code>true</code> if valid advice was found and decoded
 **/
static bool decode_uds_advice(const struct uds_request *request,
			      struct data_location *advice)
{
	if ((request->status != UDS_SUCCESS) || !request->found) {
		return false;
	}

	size_t offset = 0;
	const struct uds_chunk_data *encoding = &request->old_metadata;
	byte version = encoding->data[offset++];

	if (version != UDS_ADVICE_VERSION) {
		logError("invalid UDS advice version code %u", version);
		return false;
	}

	advice->state = encoding->data[offset++];
	decode_uint64_le(encoding->data, &offset, &advice->pbn);
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
static Jiffies get_albireo_timeout(Jiffies start_jiffies)
{
	return max_ulong(start_jiffies + albireo_timeout_jiffies,
			 jiffies + min_albireo_timer_jiffies);
}

/*****************************************************************************/
void set_albireo_timeout_interval(unsigned int value)
{
	// Arbitrary maximum value is two minutes
	if (value > 120000) {
		value = 120000;
	}
	// Arbitrary minimum value is 2 jiffies
	Jiffies alb_jiffies = msecs_to_jiffies(value);

	if (alb_jiffies < 2) {
		alb_jiffies = 2;
		value = jiffies_to_msecs(alb_jiffies);
	}
	albireo_timeout_interval = value;
	albireo_timeout_jiffies = alb_jiffies;
}

/*****************************************************************************/
void set_min_albireo_timer_interval(unsigned int value)
{
	// Arbitrary maximum value is one second
	if (value > 1000) {
		value = 1000;
	}

	// Arbitrary minimum value is 2 jiffies
	Jiffies min_jiffies = msecs_to_jiffies(value);

	if (min_jiffies < 2) {
		min_jiffies = 2;
		value = jiffies_to_msecs(min_jiffies);
	}

	min_albireo_timer_interval = value;
	min_albireo_timer_jiffies = min_jiffies;
}

/*****************************************************************************/
static void finish_index_operation(struct uds_request *uds_request)
{
	struct data_kvio *data_kvio = container_of(uds_request,
						   struct data_kvio,
						   dedupe_context.uds_request);
	struct dedupe_context *dedupe_context = &data_kvio->dedupe_context;

	if (compareAndSwap32(&dedupe_context->request_state, UR_BUSY, UR_IDLE)) {
		struct kvio *kvio = data_kvio_as_kvio(data_kvio);
		struct dedupe_index *index = kvio->layer->dedupe_index;

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
				set_dedupe_advice(dedupe_context, &advice);
			} else {
				set_dedupe_advice(dedupe_context, NULL);
			}
		}
		invoke_dedupe_callback(data_kvio);
		atomic_dec(&index->active);
	} else {
		compareAndSwap32(&dedupe_context->request_state,
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
static void start_expiration_timer_for_kvio(struct dedupe_index *index,
					    struct data_kvio *data_kvio)
{
	start_expiration_timer(index, get_albireo_timeout(
		data_kvio->dedupe_context.submission_time));
}

/*****************************************************************************/
static void start_index_operation(struct kvdo_work_item *item)
{
	struct kvio *kvio = work_item_as_kvio(item);
	struct data_kvio *data_kvio = kvio_as_data_kvio(kvio);
	struct dedupe_index *index = kvio->layer->dedupe_index;
	struct dedupe_context *dedupe_context = &data_kvio->dedupe_context;

	spin_lock_bh(&index->pending_lock);
	list_add_tail(&dedupe_context->pending_list, &index->pending_head);
	dedupe_context->is_pending = true;
	start_expiration_timer_for_kvio(index, data_kvio);
	spin_unlock_bh(&index->pending_lock);

	struct uds_request *uds_request = &dedupe_context->uds_request;
	int status = uds_start_chunk_operation(uds_request);

	if (status != UDS_SUCCESS) {
		uds_request->status = status;
		finish_index_operation(uds_request);
	}
}

/**********************************************************************/
uint64_t get_dedupe_timeout_count(struct dedupe_index *index)
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
			logDebug("UDS index timeout on %llu requests",
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
init_periodic_event_reporter(struct periodic_event_reporter *reporter,
			     struct kernel_layer *layer)
{
	INIT_WORK(&reporter->work, report_events_work);
	reporter->layer = layer;
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

/*****************************************************************************/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
static void timeout_index_operations(struct timer_list *t)
#else
static void timeout_index_operations(unsigned long arg)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
	struct dedupe_index *index = from_timer(index, t, pending_timer);
#else
	struct dedupe_index *index = (struct dedupe_index *) arg;
#endif
	LIST_HEAD(expiredHead);
	uint64_t timeout_jiffies = msecs_to_jiffies(albireo_timeout_interval);
	unsigned long earliest_submission_allowed = jiffies - timeout_jiffies;

	spin_lock_bh(&index->pending_lock);
	index->started_timer = false;
	while (!list_empty(&index->pending_head)) {
		struct data_kvio *data_kvio =
			list_first_entry(&index->pending_head,
					 struct data_kvio,
					 dedupe_context.pending_list);
		struct dedupe_context *dedupe_context =
			&data_kvio->dedupe_context;
		if (earliest_submission_allowed <=
		    dedupe_context->submission_time) {
			start_expiration_timer_for_kvio(index, data_kvio);
			break;
		}
		list_del(&dedupe_context->pending_list);
		dedupe_context->is_pending = false;
		list_add_tail(&dedupe_context->pending_list, &expiredHead);
	}
	spin_unlock_bh(&index->pending_lock);
	unsigned int timed_out = 0;
	while (!list_empty(&expiredHead)) {
		struct data_kvio *data_kvio =
			list_first_entry(&expiredHead,
					 struct data_kvio,
					 dedupe_context.pending_list);
		struct dedupe_context *dedupe_context =
			&data_kvio->dedupe_context;
		list_del(&dedupe_context->pending_list);
		if (compareAndSwap32(&dedupe_context->request_state,
				     UR_BUSY,
				     UR_TIMED_OUT)) {
			dedupe_context->status = ETIMEDOUT;
			invoke_dedupe_callback(data_kvio);
			atomic_dec(&index->active);
			timed_out++;
		}
	}
	report_dedupe_timeouts(&index->timeout_reporter, timed_out);
}

/*****************************************************************************/
static void enqueue_index_operation(struct data_kvio *data_kvio,
				    enum uds_callback_type operation)
{
	struct kvio *kvio = data_kvio_as_kvio(data_kvio);
	struct dedupe_context *dedupe_context = &data_kvio->dedupe_context;
	struct dedupe_index *index = kvio->layer->dedupe_index;

	dedupe_context->status = UDS_SUCCESS;
	dedupe_context->submission_time = jiffies;
	if (compareAndSwap32(&dedupe_context->request_state, UR_IDLE, UR_BUSY)) {
		struct uds_request *uds_request =
			&data_kvio->dedupe_context.uds_request;

		uds_request->chunk_name = *dedupe_context->chunk_name;
		uds_request->callback = finish_index_operation;
		uds_request->session = index->index_session;
		uds_request->type = operation;
		uds_request->update = true;
		if ((operation == UDS_POST) || (operation == UDS_UPDATE)) {
			encode_uds_advice(uds_request,
					  get_dedupe_advice(dedupe_context));
		}

		setup_work_item(work_item_from_kvio(kvio),
				start_index_operation,
				NULL,
				UDS_Q_ACTION);

		spin_lock(&index->state_lock);
		if (index->deduping) {
			enqueue_work_queue(index->uds_queue,
					   work_item_from_kvio(kvio));
			unsigned int active = atomic_inc_return(&index->active);

			if (active > index->maximum) {
				index->maximum = active;
			}
			kvio = NULL;
		} else {
			atomicStore32(&dedupe_context->request_state, UR_IDLE);
		}
		spin_unlock(&index->state_lock);
	} else {
		// A previous user of the kvio had a dedupe timeout
		// and its request is still outstanding.
		atomic64_inc(&kvio->layer->dedupeContextBusy);
	}
	if (kvio != NULL) {
		invoke_dedupe_callback(data_kvio);
	}
}

/*****************************************************************************/
static void close_index(struct dedupe_index *index)
{
	// Change the index state so that get_index_statistics will not try to
	// use the index session we are closing.
	index->index_state = IS_CHANGING;
	// Close the index session, while not holding the state_lock.
	spin_unlock(&index->state_lock);
	int result = uds_close_index(index->index_session);

	if (result != UDS_SUCCESS) {
		logErrorWithStringError(result,
					"Error closing index %s",
					index->index_name);
	}
	spin_lock(&index->state_lock);
	index->index_state = IS_CLOSED;
	index->error_flag |= result != UDS_SUCCESS;
	// ASSERTION: We leave in IS_CLOSED state.
}

/*****************************************************************************/
static void open_index(struct dedupe_index *index)
{
	// ASSERTION: We enter in IS_CLOSED state.
	bool create_flag = index->create_flag;

	index->create_flag = false;
	// Change the index state so that the it will be reported to the
	// outside world as "opening".
	index->index_state = IS_CHANGING;
	index->error_flag = false;
	// Open the index session, while not holding the state_lock
	spin_unlock(&index->state_lock);
	int result = uds_open_index(create_flag ? UDS_CREATE : UDS_LOAD,
				    index->index_name, &index->uds_params,
				    index->configuration,
				    index->index_session);
	if (result != UDS_SUCCESS) {
		logErrorWithStringError(result,
					"Error opening index %s",
					index->index_name);
	}
	spin_lock(&index->state_lock);
	if (!create_flag) {
		switch (result) {
		case UDS_CORRUPT_COMPONENT:
		case UDS_NO_INDEX:
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
		logInfo("Setting UDS index target state to error");
		spin_lock(&index->state_lock);
	}
	// ASSERTION: On success, we leave in IS_OPENED state.
	// ASSERTION: On failure, we leave in IS_CLOSED state.
}

/*****************************************************************************/
static void change_dedupe_state(struct kvdo_work_item *item)
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

/*****************************************************************************/
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

/*****************************************************************************/
static void set_target_state(struct dedupe_index *index,
			     index_state target,
			     bool change_dedupe,
			     bool dedupe,
			     bool set_create)
{
	spin_lock(&index->state_lock);
	const char *old_state = index_state_to_string(index,
						      index->index_target);
	if (change_dedupe) {
		index->dedupe_flag = dedupe;
	}
	if (set_create) {
		index->create_flag = true;
	}
	index->index_target = target;
	launch_dedupe_state_change(index);
	const char *new_state =
		index_state_to_string(index, index->index_target);
	spin_unlock(&index->state_lock);
	if (old_state != new_state) {
		logInfo("Setting UDS index target state to %s", new_state);
	}
}

/*****************************************************************************/
void suspend_dedupe_index(struct dedupe_index *index, bool save_flag)
{
	spin_lock(&index->state_lock);
	index->suspended = true;
	index_state index_state = index->index_state;

	spin_unlock(&index->state_lock);
	if (index_state != IS_CLOSED) {
		int result = uds_suspend_index_session(index->index_session,
						       save_flag);
		if (result != UDS_SUCCESS) {
			logErrorWithStringError(result,
						"Error suspending dedupe index");
		}
	}
}

/*****************************************************************************/
void resume_dedupe_index(struct dedupe_index *index)
{
	int result = uds_resume_index_session(index->index_session);

	if (result != UDS_SUCCESS) {
		logErrorWithStringError(result, "Error resuming dedupe index");
	}
	spin_lock(&index->state_lock);
	index->suspended = false;
	launch_dedupe_state_change(index);
	spin_unlock(&index->state_lock);
}

/*****************************************************************************/

/*****************************************************************************/
void dump_dedupe_index(struct dedupe_index *index, bool show_queue)
{
	spin_lock(&index->state_lock);
	const char *state = index_state_to_string(index, index->index_state);
	const char *target =
		(index->changing ?
			 index_state_to_string(index, index->index_target) :
			 NULL);
	spin_unlock(&index->state_lock);
	logInfo("UDS index: state: %s", state);
	if (target != NULL) {
		logInfo("UDS index: changing to state: %s", target);
	}
	if (show_queue) {
		dump_work_queue(index->uds_queue);
	}
}

/*****************************************************************************/
void finish_dedupe_index(struct dedupe_index *index)
{
	set_target_state(index, IS_CLOSED, false, false, false);
	uds_destroy_index_session(index->index_session);
	finish_work_queue(index->uds_queue);
}

/*****************************************************************************/
void free_dedupe_index(struct dedupe_index **index_ptr)
{
	if (*index_ptr == NULL) {
		return;
	}
	struct dedupe_index *index = *index_ptr;
	*index_ptr = NULL;

	free_work_queue(&index->uds_queue);
	stop_periodic_event_reporter(&index->timeout_reporter);
	spin_lock_bh(&index->pending_lock);
	if (index->started_timer) {
		del_timer_sync(&index->pending_timer);
	}
	spin_unlock_bh(&index->pending_lock);
	kobject_put(&index->dedupe_object);
}

/*****************************************************************************/
const char *get_dedupe_state_name(struct dedupe_index *index)
{
	spin_lock(&index->state_lock);
	const char *state = index_state_to_string(index, index->index_state);

	spin_unlock(&index->state_lock);
	return state;
}

/*****************************************************************************/
void get_index_statistics(struct dedupe_index *index,
			  struct index_statistics *stats)
{
	spin_lock(&index->state_lock);
	index_state index_state = index->index_state;

	stats->max_dedupe_queries = index->maximum;
	spin_unlock(&index->state_lock);
	stats->curr_dedupe_queries = atomic_read(&index->active);
	if (index_state == IS_OPENED) {
		struct uds_index_stats index_stats;
		int result = uds_get_index_stats(index->index_session,
						 &index_stats);
		if (result == UDS_SUCCESS) {
			stats->entries_indexed = index_stats.entries_indexed;
		} else {
			logErrorWithStringError(result,
						"Error reading index stats");
		}
		struct uds_context_stats context_stats;

		result = uds_get_index_session_stats(index->index_session,
					             &context_stats);
		if (result == UDS_SUCCESS) {
			stats->posts_found = context_stats.posts_found;
			stats->posts_not_found = context_stats.posts_not_found;
			stats->queries_found = context_stats.queries_found;
			stats->queries_not_found =
				context_stats.queries_not_found;
			stats->updates_found = context_stats.updates_found;
			stats->updates_not_found =
				context_stats.updates_not_found;
		} else {
			logErrorWithStringError(result,
						"Error reading context stats");
		}
	}
}


/*****************************************************************************/
int message_dedupe_index(struct dedupe_index *index, const char *name)
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

/*****************************************************************************/
void post_dedupe_advice(struct data_kvio *data_kvio)
{
	enqueue_index_operation(data_kvio, UDS_POST);
}

/*****************************************************************************/
void query_dedupe_advice(struct data_kvio *data_kvio)
{
	enqueue_index_operation(data_kvio, UDS_QUERY);
}

/*****************************************************************************/
void start_dedupe_index(struct dedupe_index *index, bool create_flag)
{
	set_target_state(index, IS_OPENED, true, true, create_flag);
}

/*****************************************************************************/
void stop_dedupe_index(struct dedupe_index *index)
{
	set_target_state(index, IS_CLOSED, false, false, false);
}

/*****************************************************************************/
void update_dedupe_advice(struct data_kvio *data_kvio)
{
	enqueue_index_operation(data_kvio, UDS_UPDATE);
}

/*****************************************************************************/
static void dedupe_kobj_release(struct kobject *kobj)
{
	struct dedupe_index *index = container_of(kobj,
						  struct dedupe_index,
						  dedupe_object);
	uds_free_configuration(index->configuration);
	FREE(index->index_name);
	FREE(index);
}

/*****************************************************************************/
static ssize_t dedupe_status_show(struct kobject *kobj,
				  struct attribute *attr,
				  char *buf)
{
	struct uds_attribute *ua =
		container_of(attr, struct uds_attribute, attr);
	struct dedupe_index *index =
		container_of(kobj, struct dedupe_index, dedupe_object);
	if (ua->show_string != NULL) {
		return sprintf(buf, "%s\n", ua->show_string(index));
	} else {
		return -EINVAL;
	}
}

/*****************************************************************************/
static ssize_t dedupe_status_store(struct kobject *kobj,
				   struct attribute *attr,
				   const char *buf,
				   size_t length)
{
	return -EINVAL;
}

/*****************************************************************************/

static struct sysfs_ops dedupeSysfsOps = {
	.show = dedupe_status_show,
	.store = dedupe_status_store,
};

static struct uds_attribute dedupeStatusAttribute = {
	.attr = {.name = "status", .mode = 0444, },
	.show_string = get_dedupe_state_name,
};

static struct attribute *dedupeAttributes[] = {
	&dedupeStatusAttribute.attr,
	NULL,
};

static struct kobj_type dedupeKobjType = {
	.release = dedupe_kobj_release,
	.sysfs_ops = &dedupeSysfsOps,
	.default_attrs = dedupeAttributes,
};

/*****************************************************************************/
static void start_uds_queue(void *ptr)
{
	/*
	 * Allow the UDS dedupe worker thread to do memory allocations.  It will
	 * only do allocations during the UDS calls that open or close an index,
	 * but those allocations can safely sleep while reserving a large amount
	 * of memory.  We could use an allocationsAllowed boolean (like the base
	 * threads do), but it would be an unnecessary embellishment.
	 */
	struct dedupe_index *index = ptr;

	register_allocating_thread(&index->allocating_thread, NULL);
}

/*****************************************************************************/
static void finish_uds_queue(void *ptr)
{
	unregister_allocating_thread();
}

/*****************************************************************************/
int make_dedupe_index(struct dedupe_index **index_ptr,
		      struct kernel_layer *layer)
{
	set_albireo_timeout_interval(albireo_timeout_interval);
	set_min_albireo_timer_interval(min_albireo_timer_interval);

	struct dedupe_index *index;
	int result = ALLOCATE(1, struct dedupe_index, "UDS index data", &index);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = allocSprintf("index name", &index->index_name,
			      "dev=%s offset=4096 size=%llu",
			      layer->device_config->parent_device_name,
			      get_index_region_size(layer->geometry) *
				      VDO_BLOCK_SIZE);
	if (result != UDS_SUCCESS) {
		logError("Creating index name failed (%d)", result);
		FREE(index);
		return result;
	}

	index->uds_params = (struct uds_parameters) UDS_PARAMETERS_INITIALIZER;
	index_config_to_uds_parameters(&layer->geometry.index_config,
				       &index->uds_params);
	result = index_config_to_uds_configuration(&layer->geometry.index_config,
						   &index->configuration);
	if (result != VDO_SUCCESS) {
		FREE(index->index_name);
		FREE(index);
		return result;
	}
	uds_configuration_set_nonce(index->configuration,
				    (uds_nonce_t) layer->geometry.nonce);

	result = uds_create_index_session(&index->index_session);
	if (result != UDS_SUCCESS) {
		uds_free_configuration(index->configuration);
		FREE(index->index_name);
		FREE(index);
		return result;
	}

	static const struct kvdo_work_queue_type uds_queue_type = {
		.start = start_uds_queue,
		.finish = finish_uds_queue,
		.action_table = {
			{ .name = "uds_action",
			  .code = UDS_Q_ACTION,
			  .priority = 0 },
		},
	};
	result = make_work_queue(layer->thread_name_prefix,
				 "dedupeQ",
				 &layer->wq_directory,
				 layer,
				 index,
				 &uds_queue_type,
				 1,
				 NULL,
				 &index->uds_queue);
	if (result != VDO_SUCCESS) {
		logError("UDS index queue initialization failed (%d)", result);
		uds_destroy_index_session(index->index_session);
		uds_free_configuration(index->configuration);
		FREE(index->index_name);
		FREE(index);
		return result;
	}

	kobject_init(&index->dedupe_object, &dedupeKobjType);
	result = kobject_add(&index->dedupe_object, &layer->kobj, "dedupe");
	if (result != VDO_SUCCESS) {
		free_work_queue(&index->uds_queue);
		uds_destroy_index_session(index->index_session);
		uds_free_configuration(index->configuration);
		FREE(index->index_name);
		FREE(index);
		return result;
	}

	INIT_LIST_HEAD(&index->pending_head);
	spin_lock_init(&index->pending_lock);
	spin_lock_init(&index->state_lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
	timer_setup(&index->pending_timer, timeout_index_operations, 0);
#else
	setup_timer(&index->pending_timer,
		    timeout_index_operations,
		    (unsigned long) index);
#endif

	// UDS Timeout Reporter
	init_periodic_event_reporter(&index->timeout_reporter,
				     layer);

	*index_ptr = index;
	return VDO_SUCCESS;
}
