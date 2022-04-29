// SPDX-License-Identifier: GPL-2.0-only
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
 */

#include "lock-counter.h"

#include <linux/atomic.h>

#include "memory-alloc.h"
#include "permassert.h"

#include "vdo.h"

/**
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
 **/
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
 * Create a lock counter.
 *
 * @param [in]  vdo               The VDO
 * @param [in]  parent            The parent to notify when the lock count goes
 *                                to zero
 * @param [in]  callback          The function to call when the lock count goes
 *                                to zero
 * @param [in]  thread_id         The id of thread on which to run the callback
 * @param [in]  logical_zones     The total number of logical zones
 * @param [in]  physical_zones    The total number of physical zones
 * @param [in]  locks             The number of locks
 * @param [out] lock_counter_ptr  A pointer to hold the new counter
 *
 * @return VDO_SUCCESS or an error
 **/
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
 * Free a lock counter.
 *
 * @param counter  The lock counter to free
 **/
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
 * Get a pointer to the zone count for a given lock on a given zone.
 *
 * @param counter      The lock counter
 * @param lock_number  The lock to get
 * @param zone_type    The zone type whose count is desired
 *
 * @return A pointer to the zone count for the given lock and zone
 **/
static inline atomic_t *get_zone_count_ptr(struct lock_counter *counter,
					   block_count_t lock_number,
					   enum vdo_zone_type zone_type)
{
	return ((zone_type == VDO_ZONE_TYPE_LOGICAL)
		? &counter->logical_zone_counts[lock_number]
		: &counter->physical_zone_counts[lock_number]);
}

/**
 * Get the zone counter for a given lock on a given zone.
 *
 * @param counter      The lock counter
 * @param lock_number  The lock to get
 * @param zone_type    The zone type whose count is desired
 * @param zone_id      The zone index whose count is desired
 *
 * @return The counter for the given lock and zone
 **/
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
 * Check whether the journal zone is locked for a given lock.
 *
 * @param counter      The lock_counter
 * @param lock_number  The lock to check
 *
 * @return <code>true</code> if the journal zone is locked
 **/
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
 * Check whether a lock is locked for a zone type. If the recovery journal has
 * a lock on the lock number, both logical and physical zones are considered
 * locked.
 *
 * @param lock_counter  The set of locks to check
 * @param lock_number   The lock to check
 * @param zone_type     The type of the zone
 *
 * @return <code>true</code> if the specified lock has references (is locked)
 **/
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
 * Check that we are on the journal thread.
 *
 * @param counter  The lock_counter
 * @param caller   The name of the caller (for logging)
 **/
static void assert_on_journal_thread(struct lock_counter *counter,
				     const char *caller)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 counter->completion.callback_thread_id),
			"%s() called from journal zone", caller);
}

/**
 * Initialize the value of the journal zone's counter for a given lock. This
 * must be called from the journal zone.
 *
 * @param counter      The counter to initialize
 * @param lock_number  Which lock to initialize
 * @param value        The value to set
 **/
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
 * Acquire a reference to a given lock in the specified zone. This method must
 * not be used from the journal zone.
 *
 * @param counter      The lock_counter
 * @param lock_number  Which lock to increment
 * @param zone_type    The type of the zone acquiring the reference
 * @param zone_id      The ID of the zone acquiring the reference
 **/
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
 * Decrement a non-atomic counter.
 *
 * @param counter      The lock_counter
 * @param lock_number  Which lock to decrement
 * @param zone_type    The type of the zone releasing the reference
 * @param zone_id      The ID of the zone releasing the reference
 *
 * @return The new value of the counter
 **/
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
 * Attempt to notify the owner of this lock_counter that some lock has been
 * released for some zone type. Will do nothing if another notification is
 * already in progress.
 *
 * @param counter  The lock_counter
 **/
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
 * Release a reference to a given lock in the specified zone. This method
 * must not be used from the journal zone.
 *
 * @param counter      The lock_counter
 * @param lock_number  Which lock to increment
 * @param zone_type    The type of the zone releasing the reference
 * @param zone_id      The ID of the zone releasing the reference
 **/
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
 * Release a single journal zone reference from the journal zone. This method
 * must be called from the journal zone.
 *
 * @param counter      The counter from which to release a reference
 * @param lock_number  The lock from which to release a reference
 **/
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
 * Release a single journal zone reference from any zone. This method shouldn't
 * be called from the journal zone as it would be inefficient; use
 * vdo_release_journal_zone_reference() instead.
 *
 * @param counter      The counter from which to release a reference
 * @param lock_number  The lock from which to release a reference
 **/
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
 * Inform a lock counter that an unlock notification was received by the
 * caller.
 *
 * @param counter  The counter to inform
 **/
void vdo_acknowledge_lock_unlock(struct lock_counter *counter)
{
	smp_wmb();
	atomic_set(&counter->state, LOCK_COUNTER_STATE_NOT_NOTIFYING);
}

/**
 * Prevent the lock counter from issuing notifications.
 *
 * @param counter  The counter
 *
 * @return <code>true</code> if the lock counter was not notifying and hence
 *         the suspend was efficacious
 **/
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
 * Re-allow notifications from a suspended lock counter.
 *
 * @param counter  The counter
 *
 * @return <code>true</code> if the lock counter was suspended
 **/
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
