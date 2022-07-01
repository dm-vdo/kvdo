// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "dedupe-index.h"

#include <asm/unaligned.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/murmurhash3.h>
#include <linux/ratelimit.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include "logger.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "uds.h"

#include "completion.h"
#include "data-vio.h"
#include "kernel-types.h"
#include "types.h"

/**
 * DOC: The dedupe index interface
 *
 * FIXME: actually write a summary of how this works with UDS.
 */

struct uds_attribute {
	struct attribute attr;
	const char *(*show_string)(struct dedupe_index *);
};

/*
 * Possible values stored in the atomic dedupe_context.request_state,
 * recording the state of the uds_request member. Note that when the
 * state is UR_TIMED_OUT, the uds_request member is still in use.
 */
enum {
	UR_IDLE,
	UR_BUSY,
	UR_TIMED_OUT,
};

/*
 * Possible index states: closed, opened, or transitioning between those two.
 */
enum index_state {
	IS_CLOSED,
	IS_CHANGING,
	IS_OPENED,
};

/*
 * A structure to manage the reporting of UDS timeouts
 */
struct periodic_event_reporter {
	uint64_t last_reported_value;
	atomic64_t value;
	struct ratelimit_state ratelimiter;
	struct work_struct work;
};

struct dedupe_index {
	struct kobject dedupe_directory;
	struct uds_parameters parameters;
	struct uds_index_session *index_session;
	atomic_t active;
	struct periodic_event_reporter timeout_reporter;
	/*
	 * This spinlock protects the state fields and the starting of dedupe
	 * requests.
	 */
	spinlock_t state_lock;
	struct vdo_completion completion; /* protected by state_lock */
	struct vdo_work_queue *uds_queue; /* protected by state_lock */
	unsigned int maximum; /* protected by state_lock */
	enum index_state index_state; /* protected by state_lock */
	enum index_state index_target; /* protected by state_lock */
	bool changing; /* protected by state_lock */
	bool create_flag; /* protected by state_lock */
	bool dedupe_flag; /* protected by state_lock */
	bool deduping; /* protected by state_lock */
	bool error_flag; /* protected by state_lock */
	bool suspended; /* protected by state_lock */

	/*
	 * This spinlock protects the pending list, the pending flag in each
	 * vio, and the timeout list.
	 */
	spinlock_t pending_lock;
	struct list_head pending_head; /* protected by pending_lock */
	struct timer_list pending_timer; /* protected by pending_lock */
	bool started_timer; /* protected by pending_lock */
};

/* Version 2 uses the kernel space UDS index and is limited to 16 bytes */
enum {
	UDS_ADVICE_VERSION = 2,
	/* version byte + state byte + 64-bit little-endian PBN */
	UDS_ADVICE_SIZE = 1 + 1 + sizeof(uint64_t),
};

static const char *CLOSED = "closed";
static const char *CLOSING = "closing";
static const char *ERROR = "error";
static const char *OFFLINE = "offline";
static const char *ONLINE = "online";
static const char *OPENING = "opening";
static const char *SUSPENDED = "suspended";
static const char *UNKNOWN = "unknown";

/* These are in milliseconds. */
unsigned int vdo_dedupe_index_timeout_interval = 5000;
unsigned int vdo_dedupe_index_min_timer_interval = 100;
/* Same two variables, in jiffies for easier consumption. */
static uint64_t vdo_dedupe_index_timeout_jiffies;
static uint64_t vdo_dedupe_index_min_timer_jiffies;

static inline struct dedupe_index *
as_dedupe_index(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type,
				   VDO_DEDUPE_INDEX_COMPLETION);
	return container_of(completion, struct dedupe_index, completion);
}

static const char *index_state_to_string(struct dedupe_index *index,
					 enum index_state state)
{
	if (index->suspended) {
		return SUSPENDED;
	}

	switch (state) {
	case IS_CLOSED:
		return index->error_flag ? ERROR : CLOSED;
	case IS_CHANGING:
		return index->index_target == IS_OPENED ? OPENING : CLOSING;
	case IS_OPENED:
		return index->dedupe_flag ? ONLINE : OFFLINE;
	default:
		return UNKNOWN;
	}
}

/*
 * Decode VDO duplicate advice from the old_metadata field of a UDS request.
 * Returns true if valid advice was found and decoded
 */
static bool decode_uds_advice(struct data_vio *data_vio,
			      const struct uds_request *request)
{
	size_t offset = 0;
	const struct uds_chunk_data *encoding = &request->old_metadata;
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct zoned_pbn *advice = &data_vio->duplicate;
	byte version;
	int result;

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

	/* Don't use advice that's clearly meaningless. */
	if ((advice->state == VDO_MAPPING_STATE_UNMAPPED) ||
	    (advice->pbn == VDO_ZERO_BLOCK)) {
		uds_log_debug("Invalid advice from deduplication server: pbn %llu, state %u. Giving up on deduplication of logical block %llu",
			      (unsigned long long) advice->pbn,
			      advice->state,
			      (unsigned long long) data_vio->logical.lbn);
		atomic64_inc(&vdo->stats.invalid_advice_pbn_count);
		return false;
	}

	result = vdo_get_physical_zone(vdo, advice->pbn, &advice->zone);
	if ((result != VDO_SUCCESS) || (advice->zone == NULL)) {
		uds_log_debug("Invalid physical block number from deduplication server: %llu, giving up on deduplication of logical block %llu",
			      (unsigned long long) advice->pbn,
			      (unsigned long long) data_vio->logical.lbn);
		atomic64_inc(&vdo->stats.invalid_advice_pbn_count);
		return false;
	}

	return true;
}

/*
 * Calculate the actual end of a timer, taking into account the absolute start
 * time and the present time.
 */
static uint64_t get_dedupe_index_timeout(uint64_t start_jiffies)
{
	return max(start_jiffies + vdo_dedupe_index_timeout_jiffies,
		   jiffies + vdo_dedupe_index_min_timer_jiffies);
}

void vdo_set_dedupe_index_timeout_interval(unsigned int value)
{
	uint64_t alb_jiffies;

	/* Arbitrary maximum value is two minutes */
	if (value > 120000) {
		value = 120000;
	}
	/* Arbitrary minimum value is 2 jiffies */
	alb_jiffies = msecs_to_jiffies(value);

	if (alb_jiffies < 2) {
		alb_jiffies = 2;
		value = jiffies_to_msecs(alb_jiffies);
	}
	vdo_dedupe_index_timeout_interval = value;
	vdo_dedupe_index_timeout_jiffies = alb_jiffies;
}

void vdo_set_dedupe_index_min_timer_interval(unsigned int value)
{
	uint64_t min_jiffies;

	/* Arbitrary maximum value is one second */
	if (value > 1000) {
		value = 1000;
	}

	/* Arbitrary minimum value is 2 jiffies */
	min_jiffies = msecs_to_jiffies(value);

	if (min_jiffies < 2) {
		min_jiffies = 2;
		value = jiffies_to_msecs(min_jiffies);
	}

	vdo_dedupe_index_min_timer_interval = value;
	vdo_dedupe_index_min_timer_jiffies = min_jiffies;
}


static void finish_index_operation(struct uds_request *request)
{
	struct data_vio *data_vio = container_of(request,
						 struct data_vio,
						 dedupe_context.uds_request);
	struct dedupe_context *context = &data_vio->dedupe_context;

	if (atomic_cmpxchg(&context->request_state, UR_BUSY, UR_IDLE) ==
	    UR_BUSY) {
		struct dedupe_index *index =
			vdo_from_data_vio(data_vio)->dedupe_index;

		spin_lock_bh(&index->pending_lock);
		if (context->is_pending) {
			list_del(&context->pending_list);
			context->is_pending = false;
		}
		spin_unlock_bh(&index->pending_lock);

		context->status = request->status;
		if ((request->type == UDS_POST) ||
		    (request->type == UDS_QUERY)) {
			data_vio->is_duplicate =
				decode_uds_advice(data_vio, request);
		}

		continue_data_vio(data_vio, VDO_SUCCESS);
		atomic_dec(&index->active);
	} else {
		atomic_cmpxchg(&context->request_state,
			       UR_TIMED_OUT,
			       UR_IDLE);
	}
}

/*
 * Must be called holding pending_lock
 */
static void start_expiration_timer(struct dedupe_index *index,
				   unsigned long expiration)
{
	if (!index->started_timer) {
		index->started_timer = true;
		mod_timer(&index->pending_timer, expiration);
	}
}

/*
 * Must be called holding pending_lock
 */
static void start_expiration_timer_for_vio(struct dedupe_index *index,
					   struct data_vio *data_vio)
{
	struct dedupe_context *context = &data_vio->dedupe_context;
	uint64_t start_time = context->submission_jiffies;

	start_expiration_timer(index, get_dedupe_index_timeout(start_time));
}

uint64_t vdo_get_dedupe_index_timeout_count(struct dedupe_index *index)
{
	return atomic64_read(&index->timeout_reporter.value);
}

static void report_events(struct periodic_event_reporter *reporter,
			  bool ratelimit)
{
	uint64_t new_value = atomic64_read(&reporter->value);
	uint64_t difference = new_value - reporter->last_reported_value;

	if (difference != 0) {
		if (!ratelimit || __ratelimit(&reporter->ratelimiter)) {
			uds_log_debug("UDS index timeout on %llu requests",
				      (unsigned long long) difference);
			reporter->last_reported_value = new_value;
		} else {
			/*
			 * Turn on a backup timer that will fire after the
			 * current interval. Just in case the last index
			 * request in a while times out; we want to report
			 * the dedupe timeouts in a timely manner in such cases
			 */
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

static void report_events_work(struct work_struct *work)
{
	struct periodic_event_reporter *reporter =
		container_of(work, struct periodic_event_reporter, work);
	report_events(reporter, true);
}

static void
init_periodic_event_reporter(struct periodic_event_reporter *reporter)
{
	INIT_WORK(&reporter->work, report_events_work);
	ratelimit_default_init(&reporter->ratelimiter);
	/*
	 * Since we will save up the timeouts that would have been reported
	 * but were ratelimited, we don't need to report ratelimiting.
	 */
	ratelimit_set_flags(&reporter->ratelimiter, RATELIMIT_MSG_ON_RELEASE);
}

/*
 * Record and eventually report that some dedupe requests reached their
 * expiration time without getting answers, so we timed them out.
 *
 * This is called in a timer context, so it shouldn't do the reporting
 * directly.
 */
static void report_dedupe_timeouts(struct periodic_event_reporter *reporter,
				   unsigned int timeouts)
{
	atomic64_add(timeouts, &reporter->value);
	/* If it's already queued, requeueing it will do nothing. */
	schedule_work(&reporter->work);
}

static void
stop_periodic_event_reporter(struct periodic_event_reporter *reporter)
{
	cancel_work_sync(&reporter->work);
	report_events(reporter, false);
	ratelimit_state_exit(&reporter->ratelimiter);
}

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
			continue_data_vio(data_vio, VDO_SUCCESS);
			atomic_dec(&index->active);
			timed_out++;
		}
	}
	report_dedupe_timeouts(&index->timeout_reporter, timed_out);
}

static void prepare_uds_request(struct uds_request *request,
				struct data_vio *data_vio,
				struct uds_index_session *session,
				enum uds_request_type operation)
{
	request->chunk_name = data_vio->chunk_name;
	request->callback = finish_index_operation;
	request->session = session;
	request->type = operation;
	if ((operation == UDS_POST) || (operation == UDS_UPDATE)) {
		size_t offset = 0;
		struct uds_chunk_data *encoding = &request->new_metadata;

		encoding->data[offset++] = UDS_ADVICE_VERSION;
		encoding->data[offset++] = data_vio->new_mapped.state;
		put_unaligned_le64(data_vio->new_mapped.pbn,
				   &encoding->data[offset]);
		offset += sizeof(uint64_t);
		BUG_ON(offset != UDS_ADVICE_SIZE);
	}
}

/*
 * The index operation will inquire about data_vio.chunk_name, providing (if
 * the operation is appropriate) advice from the data_vio's new_mapped
 * fields. The advice found in the index (or NULL if none) will be returned via
 * receive_data_vio_dedupe_advice(). dedupe_context.status is set to the return
 * status code of any asynchronous index processing.
 */
void vdo_query_index(struct data_vio *data_vio,
		     enum uds_request_type operation)
{
	struct dedupe_context *context = &data_vio->dedupe_context;
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct dedupe_index *index = vdo->dedupe_index;
	struct uds_request *request;
	unsigned int active;
	int result;

	vdo_assert_on_dedupe_thread(vdo, __func__);
	context->status = UDS_SUCCESS;
	context->submission_jiffies = jiffies;
	request = &context->uds_request;
	prepare_uds_request(request,
			    data_vio,
			    index->index_session,
			    operation);
	active = atomic_inc_return(&index->active);
	if (active > index->maximum) {
		index->maximum = active;
	}

	spin_lock_bh(&index->pending_lock);
	list_add_tail(&context->pending_list, &index->pending_head);
	context->is_pending = true;
	start_expiration_timer_for_vio(index, data_vio);
	spin_unlock_bh(&index->pending_lock);

	result = uds_start_chunk_operation(request);
	if (result != UDS_SUCCESS) {
		request->status = result;
		finish_index_operation(request);
	}
}

static void close_index(struct dedupe_index *index)
{
	int result;

	/*
	 * Change the index state so that vdo_get_dedupe_index_statistics will
	 * not try to use the index session we are closing.
	 */
	index->index_state = IS_CHANGING;
	/* Close the index session, while not holding the state_lock. */
	spin_unlock(&index->state_lock);
	result = uds_close_index(index->index_session);

	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "Error closing index");
	}
	spin_lock(&index->state_lock);
	index->index_state = IS_CLOSED;
	index->error_flag |= result != UDS_SUCCESS;
	/* ASSERTION: We leave in IS_CLOSED state. */
}

static void open_index(struct dedupe_index *index)
{
	/* ASSERTION: We enter in IS_CLOSED state. */
	int result;
	bool create_flag = index->create_flag;

	index->create_flag = false;
	/*
	 * Change the index state so that the it will be reported to the
	 * outside world as "opening".
	 */
	index->index_state = IS_CHANGING;
	index->error_flag = false;
	/* Open the index session, while not holding the state_lock */
	spin_unlock(&index->state_lock);
	result = uds_open_index(create_flag ? UDS_CREATE : UDS_LOAD,
				&index->parameters,
				index->index_session);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "Error opening index");
	}
	spin_lock(&index->state_lock);
	if (!create_flag) {
		switch (result) {
		case -ENOENT:
			/*
			 * Either there is no index, or there is no way we can
			 * recover the index. We will be called again and try
			 * to create a new index.
			 */
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
	/*
	 * ASSERTION: On success, we leave in IS_OPENED state.
	 * ASSERTION: On failure, we leave in IS_CLOSED state.
	 */
}

static void change_dedupe_state(struct vdo_completion *completion)
{
	struct dedupe_index *index = as_dedupe_index(completion);

	spin_lock(&index->state_lock);

	/*
	 * Loop until the index is in the target state and the create flag is
	 * clear.
	 */
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

static void launch_dedupe_state_change(struct dedupe_index *index)
{
	/* ASSERTION: We enter with the state_lock held. */
	if (index->changing || index->suspended) {
		/*
		 * Either a change is already in progress, or changes are
		 * not allowed.
		 */
		return;
	}

	if (index->create_flag ||
	    (index->index_state != index->index_target)) {
		index->changing = true;
		index->deduping = false;
		vdo_invoke_completion_callback(&index->completion);
		return;
	}

	/* Online vs. offline changes happen immediately */
	index->deduping = (index->dedupe_flag && !index->suspended &&
			   (index->index_state == IS_OPENED));

	/* ASSERTION: We exit with the state_lock held. */
}

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

/*
 * May be called from any thread.
 * save_flag should be true to save the index instead of just suspend.
 */
void vdo_suspend_dedupe_index(struct dedupe_index *index, bool save_flag)
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

/*
 * May be called from any thread.
 */
void vdo_resume_dedupe_index(struct dedupe_index *index,
			     struct device_config *config)
{
	int result;

	index->parameters.name = config->parent_device_name;
	result = uds_resume_index_session(index->index_session,
					  index->parameters.name);

	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "Error resuming dedupe index");
	}

	spin_lock(&index->state_lock);
	index->suspended = false;

	if (config->deduplication) {
		index->index_target = IS_OPENED;
		index->dedupe_flag = true;
	} else {
		index->index_target = IS_CLOSED;
	}

	launch_dedupe_state_change(index);
	spin_unlock(&index->state_lock);
}

/*
 * Do the dedupe section of dmsetup message vdo0 0 dump ...
 */
void vdo_dump_dedupe_index(struct dedupe_index *index)
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
}

void vdo_finish_dedupe_index(struct dedupe_index *index)
{
	if (index == NULL) {
		return;
	}

	uds_destroy_index_session(index->index_session);
	finish_work_queue(index->uds_queue);
}

void vdo_free_dedupe_index(struct dedupe_index *index)
{
	if (index == NULL) {
		return;
	}

	/*
	 * The queue will get freed along with all the others, but give up
	 * our reference to it.
	 */
	UDS_FORGET(index->uds_queue);
	stop_periodic_event_reporter(&index->timeout_reporter);
	spin_lock_bh(&index->pending_lock);
	if (index->started_timer) {
		del_timer_sync(&index->pending_timer);
	}
	spin_unlock_bh(&index->pending_lock);
	kobject_put(&index->dedupe_directory);
}

const char *vdo_get_dedupe_index_state_name(struct dedupe_index *index)
{
	const char *state;

	spin_lock(&index->state_lock);
	state = index_state_to_string(index, index->index_state);
	spin_unlock(&index->state_lock);

	return state;
}

void vdo_get_dedupe_index_statistics(struct dedupe_index *index,
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


/*
 * Handle a dmsetup message relevant to the index.
 */
int vdo_message_dedupe_index(struct dedupe_index *index, const char *name)
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

int vdo_add_dedupe_index_sysfs(struct dedupe_index *index,
			       struct kobject *parent)
{
	return kobject_add(&index->dedupe_directory, parent, "dedupe");
}

/*
 * If create_flag, create a new index without first attempting to load an
 * existing index.
 */
void vdo_start_dedupe_index(struct dedupe_index *index, bool create_flag)
{
	set_target_state(index, IS_OPENED, true, true, create_flag);
}

static void dedupe_kobj_release(struct kobject *directory)
{
	struct dedupe_index *index = container_of(directory,
						  struct dedupe_index,
						  dedupe_directory);
	UDS_FREE(index);
}

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

static ssize_t dedupe_status_store(struct kobject *kobj __always_unused,
				   struct attribute *attr __always_unused,
				   const char *buf __always_unused,
				   size_t length __always_unused)
{
	return -EINVAL;
}

/*----------------------------------------------------------------------*/

static struct sysfs_ops dedupe_sysfs_ops = {
	.show = dedupe_status_show,
	.store = dedupe_status_store,
};

static struct uds_attribute dedupe_status_attribute = {
	.attr = {.name = "status", .mode = 0444, },
	.show_string = vdo_get_dedupe_index_state_name,
};

static struct attribute *dedupe_attrs[] = {
	&dedupe_status_attribute.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dedupe);

static struct kobj_type dedupe_directory_type = {
	.release = dedupe_kobj_release,
	.sysfs_ops = &dedupe_sysfs_ops,
	.default_groups = dedupe_groups,
};

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
	struct vdo_thread *thread
		= get_work_queue_owner(get_current_work_queue());

	uds_register_allocating_thread(&thread->allocating_thread, NULL);
}

static void finish_uds_queue(void *ptr __always_unused)
{
	uds_unregister_allocating_thread();
}

int vdo_make_dedupe_index(struct vdo *vdo, struct dedupe_index **index_ptr)
{
	int result;
	off_t uds_offset;
	struct dedupe_index *index;
	static const struct vdo_work_queue_type uds_queue_type = {
		.start = start_uds_queue,
		.finish = finish_uds_queue,
		.max_priority = UDS_Q_MAX_PRIORITY,
		.default_priority = UDS_Q_PRIORITY,
	};
	vdo_set_dedupe_index_timeout_interval(vdo_dedupe_index_timeout_interval);
	vdo_set_dedupe_index_min_timer_interval(vdo_dedupe_index_min_timer_interval);

	result = UDS_ALLOCATE(1, struct dedupe_index, "UDS index data", &index);

	if (result != UDS_SUCCESS) {
		return result;
	}

	uds_offset = ((vdo_get_index_region_start(vdo->geometry) -
		       vdo->geometry.bio_offset) * VDO_BLOCK_SIZE);
	index->parameters.name = vdo->device_config->parent_device_name;
	index->parameters.offset = uds_offset;
	index->parameters.size =
		vdo_get_index_region_size(vdo->geometry) * VDO_BLOCK_SIZE;
	index->parameters.memory_size = vdo->geometry.index_config.mem;
	index->parameters.sparse = vdo->geometry.index_config.sparse;
	index->parameters.nonce = (uint64_t) vdo->geometry.nonce;

	result = uds_create_index_session(&index->index_session);
	if (result != UDS_SUCCESS) {
		UDS_FREE(index);
		return result;
	}

	result = vdo_make_thread(vdo,
				 vdo->thread_config->dedupe_thread,
				 &uds_queue_type,
				 1,
				 NULL);
	if (result != VDO_SUCCESS) {
		uds_log_error("UDS index queue initialization failed (%d)",
			  result);
		uds_destroy_index_session(index->index_session);
		UDS_FREE(index);
		return result;
	}

	vdo_initialize_completion(&index->completion,
				  vdo,
				  VDO_DEDUPE_INDEX_COMPLETION);
	vdo_set_completion_callback(&index->completion,
				    change_dedupe_state,
				    vdo->thread_config->dedupe_thread);
	index->uds_queue
		= vdo->threads[vdo->thread_config->dedupe_thread].queue;
	kobject_init(&index->dedupe_directory, &dedupe_directory_type);
	INIT_LIST_HEAD(&index->pending_head);
	spin_lock_init(&index->pending_lock);
	spin_lock_init(&index->state_lock);
	timer_setup(&index->pending_timer, timeout_index_operations, 0);

	init_periodic_event_reporter(&index->timeout_reporter);

	*index_ptr = index;
	return VDO_SUCCESS;
}

bool data_vio_may_query_index(struct data_vio *data_vio)
{
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct dedupe_index *index = vdo->dedupe_index;
	bool deduping;

	spin_lock(&index->state_lock);
	deduping = index->deduping;
	spin_unlock(&index->state_lock);

	if (!deduping) {
		return false;
	}

	if (atomic_cmpxchg(&data_vio->dedupe_context.request_state,
			   UR_IDLE, UR_BUSY) != UR_IDLE) {
		/*
		 * A previous user of the data_vio had a dedupe timeout
		 * and its request is still outstanding.
		 */
		atomic64_inc(&vdo->stats.dedupe_context_busy);
		return false;
	}

	return true;
}
