// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "lock-counter.h"

#include <linux/atomic.h>

#include "memory-alloc.h"
#include "permassert.h"

#include "vdo.h"

/**
 * DOC:
 *
 * A lock_counter is intended to keep all of the locks for the blocks in the
 * recovery journal. The per-zone counters are all kept in a single array which
 * is arranged by zone (i.e. zone 0's lock 0 is at index 0, zone 0's lock 1 is
 * at index 1, and zone 1's lock 0 is at index 'locks'.  This arrangement is
 * intended to minimize cache-line contention for counters from different
 * zones.
 *
 * The locks are implemented as a single object instead of as a lock counter
 * per lock both to afford this opportunity to reduce cache line contention and
 * also to eliminate the need to have a completion per lock.
 *
 * Lock sets are laid out with the set for recovery journal first, followed by
 * the logical zones, and then the physical zones.
 */

enum lock_counter_state {
	LOCK_COUNTER_STATE_NOT_NOTIFYING,
	LOCK_COUNTER_STATE_NOTIFYING,
	LOCK_COUNTER_STATE_SUSPENDED,
};

struct lock_counter {
	/** The completion for notifying the owner of a lock release */
	struct vdo_completion completion;
	/** The number of logical zones which may hold locks */
	zone_count_t logical_zones;
	/** The number of physical zones which may hold locks */
	zone_count_t physical_zones;
	/** The number of locks */
	block_count_t locks;
	/** Whether the lock release notification is in flight */
	atomic_t state;
	/** The number of logical zones which hold each lock */
	atomic_t *logical_zone_counts;
	/** The number of physical zones which hold each lock */
	atomic_t *physical_zone_counts;
	/** The per-zone, per-lock counts for the journal zone */
	uint16_t *journal_counters;
	/** The per-zone, per-lock decrement counts for the journal zone */
	atomic_t *journal_decrement_counts;
	/** The per-zone, per-lock reference counts for logical zones */
	uint16_t *logical_counters;
	/** The per-zone, per-lock reference counts for physical zones */
	uint16_t *physical_counters;
};

/**
 * vdo_make_lock_counter() - Create a lock counter.
 *
 * @vdo: The VDO.
 * @parent: The parent to notify when the lock count goes to zero.
 * @callback: The function to call when the lock count goes to zero.
 * @thread_id: The id of thread on which to run the callback.
 * @logical_zones: The total number of logical zones.
 * @physical_zones: The total number of physical zones.
 * @locks: The number of locks.
 * @lock_counter_ptr: A pointer to hold the new counter.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_make_lock_counter(struct vdo *vdo,
			  void *parent,
			  vdo_action callback,
			  thread_id_t thread_id,
			  zone_count_t logical_zones,
			  zone_count_t physical_zones,
			  block_count_t locks,
			  struct lock_counter **lock_counter_ptr)
{
	struct lock_counter *lock_counter;

	int result = UDS_ALLOCATE(1, struct lock_counter, __func__, &lock_counter);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(locks, uint16_t, __func__,
			      &lock_counter->journal_counters);
	if (result != VDO_SUCCESS) {
		vdo_free_lock_counter(lock_counter);
		return result;
	}

	result = UDS_ALLOCATE(locks, atomic_t, __func__,
			      &lock_counter->journal_decrement_counts);
	if (result != VDO_SUCCESS) {
		vdo_free_lock_counter(lock_counter);
		return result;
	}

	result = UDS_ALLOCATE(locks * logical_zones, uint16_t, __func__,
			      &lock_counter->logical_counters);
	if (result != VDO_SUCCESS) {
		vdo_free_lock_counter(lock_counter);
		return result;
	}

	result = UDS_ALLOCATE(locks, atomic_t, __func__,
			      &lock_counter->logical_zone_counts);
	if (result != VDO_SUCCESS) {
		vdo_free_lock_counter(lock_counter);
		return result;
	}

	result = UDS_ALLOCATE(locks * physical_zones, uint16_t, __func__,
			      &lock_counter->physical_counters);
	if (result != VDO_SUCCESS) {
		vdo_free_lock_counter(lock_counter);
		return result;
	}

	result = UDS_ALLOCATE(locks, atomic_t, __func__,
			      &lock_counter->physical_zone_counts);
	if (result != VDO_SUCCESS) {
		vdo_free_lock_counter(lock_counter);
		return result;
	}

	vdo_initialize_completion(&lock_counter->completion, vdo,
				  VDO_LOCK_COUNTER_COMPLETION);
	vdo_set_completion_callback_with_parent(&lock_counter->completion,
						callback,
						thread_id,
						parent);
	lock_counter->logical_zones = logical_zones;
	lock_counter->physical_zones = physical_zones;
	lock_counter->locks = locks;
	*lock_counter_ptr = lock_counter;
	return VDO_SUCCESS;
}

/**
 * vdo_free_lock_counter() - Free a lock counter.
 * @counter: The lock counter to free.
 */
void vdo_free_lock_counter(struct lock_counter *counter)
{
	if (counter == NULL) {
		return;
	}

	UDS_FREE(UDS_FORGET(counter->physical_zone_counts));
	UDS_FREE(UDS_FORGET(counter->logical_zone_counts));
	UDS_FREE(UDS_FORGET(counter->journal_decrement_counts));
	UDS_FREE(UDS_FORGET(counter->journal_counters));
	UDS_FREE(UDS_FORGET(counter->logical_counters));
	UDS_FREE(UDS_FORGET(counter->physical_counters));
	UDS_FREE(counter);
}

/**
 * get_zone_count_ptr() - Get a pointer to the zone count for a given lock
 *                        on a given zone.
 * @counter: The lock counter.
 * @lock_number: The lock to get.
 * @zone_type: The zone type whose count is desired.
 *
 * Return: A pointer to the zone count for the given lock and zone.
 */
static inline atomic_t *get_zone_count_ptr(struct lock_counter *counter,
					   block_count_t lock_number,
					   enum vdo_zone_type zone_type)
{
	return ((zone_type == VDO_ZONE_TYPE_LOGICAL)
		? &counter->logical_zone_counts[lock_number]
		: &counter->physical_zone_counts[lock_number]);
}

/**
 * get_counter() - Get the zone counter for a given lock on a given zone.
 * @counter: The lock counter.
 * @lock_number: The lock to get.
 * @zone_type: The zone type whose count is desired.
 * @zone_id: The zone index whose count is desired.
 *
 * Return: The counter for the given lock and zone.
 */
static inline uint16_t *get_counter(struct lock_counter *counter,
				    block_count_t lock_number,
				    enum vdo_zone_type zone_type,
				    zone_count_t zone_id)
{
	block_count_t zone_counter = (counter->locks * zone_id) + lock_number;

	if (zone_type == VDO_ZONE_TYPE_JOURNAL) {
		return &counter->journal_counters[zone_counter];
	}

	if (zone_type == VDO_ZONE_TYPE_LOGICAL) {
		return &counter->logical_counters[zone_counter];
	}

	return &counter->physical_counters[zone_counter];
}

/**
 * is_journal_zone_locked() - Check whether the journal zone is locked for
 *                            a given lock.
 * @counter: The lock_counter.
 * @lock_number: The lock to check.
 *
 * Return: true if the journal zone is locked.
 */
static bool is_journal_zone_locked(struct lock_counter *counter,
				   block_count_t lock_number)
{
	uint16_t journal_value =
		*(get_counter(counter, lock_number, VDO_ZONE_TYPE_JOURNAL, 0));
	uint32_t decrements =
		atomic_read(&(counter->journal_decrement_counts[lock_number]));
	smp_rmb();
	ASSERT_LOG_ONLY((decrements <= journal_value),
			"journal zone lock counter must not underflow");

	return (journal_value != decrements);
}

/**
 * vdo_is_lock_locked() - Check whether a lock is locked for a zone type.
 * @lock_counter: The set of locks to check.
 * @lock_number: The lock to check.
 * @zone_type: The type of the zone.
 * 
 * If the recovery journal has a lock on the lock number, both logical
 * and physical zones are considered locked.
 *
 * Return: true if the specified lock has references (is locked).
 */
bool vdo_is_lock_locked(struct lock_counter *lock_counter,
			block_count_t lock_number,
			enum vdo_zone_type zone_type)
{
	atomic_t *zone_count;
	bool locked;

	ASSERT_LOG_ONLY((zone_type != VDO_ZONE_TYPE_JOURNAL),
			"vdo_is_lock_locked() called for non-journal zone");
	if (is_journal_zone_locked(lock_counter, lock_number)) {
		return true;
	}

	zone_count = get_zone_count_ptr(lock_counter, lock_number, zone_type);
	locked = (atomic_read(zone_count) != 0);
	smp_rmb();
	return locked;
}

/**
 * assert_on_journal_thread() - Check that we are on the journal thread.
 * @counter: The lock_counter.
 * @caller: The name of the caller (for logging).
 */
static void assert_on_journal_thread(struct lock_counter *counter,
				     const char *caller)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 counter->completion.callback_thread_id),
			"%s() called from journal zone", caller);
}

/**
 * vdo_initialize_lock_count() - Initialize the value of the journal zone's
 *                               counter for a given lock.
 * @counter: The counter to initialize.
 * @lock_number: Which lock to initialize.
 * @value: The value to set.
 *
 * Context: This must be called from the journal zone.
 */
void vdo_initialize_lock_count(struct lock_counter *counter,
			       block_count_t lock_number,
			       uint16_t value)
{
	uint16_t *journal_value;
	atomic_t *decrement_count;

	assert_on_journal_thread(counter, __func__);
	journal_value =
		get_counter(counter, lock_number, VDO_ZONE_TYPE_JOURNAL, 0);
	decrement_count = &(counter->journal_decrement_counts[lock_number]);
	ASSERT_LOG_ONLY((*journal_value == atomic_read(decrement_count)),
			"count to be initialized not in use");

	*journal_value = value;
	atomic_set(decrement_count, 0);
}

/**
 * vdo_acquire_lock_count_reference() - Acquire a reference to a given lock
 *                                      in the specified zone.
 * @counter: The lock_counter.
 * @lock_number: Which lock to increment.
 * @zone_type: The type of the zone acquiring the reference.
 * @zone_id: The ID of the zone acquiring the reference.
 *
 * Context: This method must not be used from the journal zone.
 */
void vdo_acquire_lock_count_reference(struct lock_counter *counter,
				      block_count_t lock_number,
				      enum vdo_zone_type zone_type,
				      zone_count_t zone_id)
{
	uint16_t *current_value;

	ASSERT_LOG_ONLY((zone_type != VDO_ZONE_TYPE_JOURNAL),
			"invalid lock count increment from journal zone");

	current_value = get_counter(counter, lock_number, zone_type, zone_id);
	ASSERT_LOG_ONLY(*current_value < UINT16_MAX,
			"increment of lock counter must not overflow");

	if (*current_value == 0) {
		/*
		 * This zone is acquiring this lock for the first time.
		 * Extra barriers because this was original developed using
		 * an atomic add operation that implicitly had them.
		 */
		smp_mb__before_atomic();
		atomic_inc(get_zone_count_ptr(counter, lock_number,
					      zone_type));
		smp_mb__after_atomic();
	}
	*current_value += 1;
}

/**
 * release_reference() - Decrement a non-atomic counter.
 * @counter: The lock_counter.
 * @lock_number: Which lock to decrement.
 * @zone_type: The type of the zone releasing the reference.
 * @zone_id: The ID of the zone releasing the reference.
 *
 * Return: The new value of the counter.
 */
static uint16_t release_reference(struct lock_counter *counter,
				  block_count_t lock_number,
				  enum vdo_zone_type zone_type,
				  zone_count_t zone_id)
{
	uint16_t *current_value =
		get_counter(counter, lock_number, zone_type, zone_id);
	ASSERT_LOG_ONLY((*current_value >= 1),
			"decrement of lock counter must not underflow");

	*current_value -= 1;
	return *current_value;
}

/**
 * attempt_notification() - Attempt to notify the owner of this lock_counter
 *                          that some lock has been released for some zone
 *                          type.
 * @counter: The lock_counter.
 *
 * Will do nothing if another notification is already in progress.
 */
static void attempt_notification(struct lock_counter *counter)
{
	int prior_state;

	/*
	 * Extra barriers because this was original developed using
	 * a CAS operation that implicitly had them.
	 */
	smp_mb__before_atomic();
	prior_state = atomic_cmpxchg(&counter->state,
				     LOCK_COUNTER_STATE_NOT_NOTIFYING,
				     LOCK_COUNTER_STATE_NOTIFYING);
	smp_mb__after_atomic();

	if (prior_state != LOCK_COUNTER_STATE_NOT_NOTIFYING) {
		return;
	}

	vdo_reset_completion(&counter->completion);
	vdo_invoke_completion_callback(&counter->completion);
}

/**
 * vdo_release_lock_count_reference() - Release a reference to a given lock
 *                                      in the specified zone.
 * @counter: The lock_counter.
 * @lock_number: Which lock to increment.
 * @zone_type: The type of the zone releasing the reference.
 * @zone_id: The ID of the zone releasing the reference.
 *
 * Context: This method must not be used from the journal zone.
 */
void vdo_release_lock_count_reference(struct lock_counter *counter,
				      block_count_t lock_number,
				      enum vdo_zone_type zone_type,
				      zone_count_t zone_id)
{
	atomic_t *zone_count;

	ASSERT_LOG_ONLY((zone_type != VDO_ZONE_TYPE_JOURNAL),
			"invalid lock count decrement from journal zone");
	if (release_reference(counter, lock_number, zone_type, zone_id) != 0) {
		return;
	}

	zone_count = get_zone_count_ptr(counter, lock_number, zone_type);
	if (atomic_add_return(-1, zone_count) == 0) {
		/*
		 * This zone was the last lock holder of its type, so try to
		 * notify the owner.
		 */
		attempt_notification(counter);
	}
}

/**
 * vdo_release_journal_zone_reference() - Release a single journal zone
 *                                        reference from the journal zone.
 * @counter: The counter from which to release a reference.
 * @lock_number: The lock from which to release a reference.
 *
 * Context: This method must be called from the journal zone.
 */
void vdo_release_journal_zone_reference(struct lock_counter *counter,
					block_count_t lock_number)
{
	assert_on_journal_thread(counter, __func__);
	release_reference(counter, lock_number, VDO_ZONE_TYPE_JOURNAL, 0);
	if (!is_journal_zone_locked(counter, lock_number)) {
		/* The journal zone is not locked, so try to notify the owner. */
		attempt_notification(counter);
	}
}

/**
 * vdo_release_journal_zone_reference_from_other_zone() - Release a single
 *                                                        journal zone
 *                                                        reference from any
 *                                                        zone.
 * @counter: The counter from which to release a reference.
 * @lock_number: The lock from which to release a reference.
 *
 * Context: This method shouldn't be called from the journal zone as
 * it would be inefficient; use vdo_release_journal_zone_reference()
 * instead.
 */
void
vdo_release_journal_zone_reference_from_other_zone(struct lock_counter *counter,
						   block_count_t lock_number)
{
	/*
	 * Extra barriers because this was original developed using
	 * an atomic add operation that implicitly had them.
	 */
	smp_mb__before_atomic();
	atomic_inc(&(counter->journal_decrement_counts[lock_number]));
	smp_mb__after_atomic();
}

/**
 * vdo_acknowledge_lock_unlock() - Inform a lock counter that an unlock
 *                                 notification was received by the caller.
 *
 * @counter: The counter to inform.
 */
void vdo_acknowledge_lock_unlock(struct lock_counter *counter)
{
	smp_wmb();
	atomic_set(&counter->state, LOCK_COUNTER_STATE_NOT_NOTIFYING);
}

/**
 * vdo_suspend_lock_counter() - Prevent the lock counter from issuing
 *                              notifications.
 * @counter: The counter.
 *
 * Return: true if the lock counter was not notifying and hence
 *         the suspend was efficacious.
 */
bool vdo_suspend_lock_counter(struct lock_counter *counter)
{
	int prior_state;

	assert_on_journal_thread(counter, __func__);

	/*
	 * Extra barriers because this was original developed using
	 * a CAS operation that implicitly had them.
	 */
	smp_mb__before_atomic();
	prior_state = atomic_cmpxchg(&counter->state,
				     LOCK_COUNTER_STATE_NOT_NOTIFYING,
				     LOCK_COUNTER_STATE_SUSPENDED);
	smp_mb__after_atomic();

	return ((prior_state == LOCK_COUNTER_STATE_SUSPENDED)
		|| (prior_state == LOCK_COUNTER_STATE_NOT_NOTIFYING));
}

/**
 * vdo_resume_lock_counter() - Re-allow notifications from a suspended lock
 *                             counter.
 * @counter: The counter.
 *
 * Return: true if the lock counter was suspended.
 */
bool vdo_resume_lock_counter(struct lock_counter *counter)
{
	int prior_state;

	assert_on_journal_thread(counter, __func__);

	/*
	 * Extra barriers because this was original developed using
	 * a CAS operation that implicitly had them.
	 */
	smp_mb__before_atomic();
	prior_state = atomic_cmpxchg(&counter->state,
				     LOCK_COUNTER_STATE_SUSPENDED,
				     LOCK_COUNTER_STATE_NOT_NOTIFYING);
	smp_mb__after_atomic();

	return (prior_state == LOCK_COUNTER_STATE_SUSPENDED);
}
