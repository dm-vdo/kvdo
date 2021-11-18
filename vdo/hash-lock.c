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

/**
 * A hash_lock controls and coordinates writing, index access, and dedupe among
 * groups of data_vios concurrently writing identical blocks, allowing them to
 * deduplicate not only against advice but also against each other. This save
 * on index queries and allows those data_vios to concurrently deduplicate
 * against a single block instead of being serialized through a PBN read lock.
 * Only one index query is needed for each hash_lock, instead of one for every
 * data_vio.
 *
 * A hash_lock acts like a state machine perhaps more than as a lock. Other
 * than the starting and ending states INITIALIZING and DESTROYING, every
 * state represents and is held for the duration of an asynchronous operation.
 * All state transitions are performed on the thread of the hash_zone
 * containing the lock. An asynchronous operation is almost always performed
 * upon entering a state, and the callback from that operation triggers
 * exiting the state and entering a new state.
 *
 * In all states except DEDUPING, there is a single data_vio, called the lock
 * agent, performing the asynchronous operations on behalf of the lock. The
 * agent will change during the lifetime of the lock if the lock is shared by
 * more than one data_vio. data_vios waiting to deduplicate are kept on a wait
 * queue. Viewed a different way, the agent holds the lock exclusively until
 * the lock enters the DEDUPING state, at which point it becomes a shared lock
 * that all the waiters (and any new data_vios that arrive) use to share a PBN
 * lock. In state DEDUPING, there is no agent. When the last data_vio in the
 * lock calls back in DEDUPING, it becomes the agent and the lock becomes
 * exclusive again. New data_vios that arrive in the lock will also go on the
 * wait queue.
 *
 * The existence of lock waiters is a key factor controlling which state the
 * lock transitions to next. When the lock is new or has waiters, it will
 * always try to reach DEDUPING, and when it doesn't, it will try to clean up
 * and exit.
 *
 * Deduping requires holding a PBN lock on a block that is known to contain
 * data identical to the data_vios in the lock, so the lock will send the
 * agent to the duplicate zone to acquire the PBN lock (LOCKING), to the
 * kernel I/O threads to read and verify the data (VERIFYING), or to write a
 * new copy of the data to a full data block or a slot in a compressed block
 * (WRITING).
 *
 * Cleaning up consists of updating the index when the data location is
 * different from the initial index query (UPDATING, triggered by stale
 * advice, compression, and rollover), releasing the PBN lock on the duplicate
 * block (UNLOCKING), and releasing the hash_lock itself back to the hash zone
 * (DESTROYING).
 *
 * The shortest sequence of states is for non-concurrent writes of new data:
 *   INITIALIZING -> QUERYING -> WRITING -> DESTROYING
 * This sequence is short because no PBN read lock or index update is needed.
 *
 * Non-concurrent, finding valid advice looks like this (endpoints elided):
 *   -> QUERYING -> LOCKING -> VERIFYING -> DEDUPING -> UNLOCKING ->
 * Or with stale advice (endpoints elided):
 *   -> QUERYING -> LOCKING -> VERIFYING -> UNLOCKING -> WRITING -> UPDATING ->
 *
 * When there are not enough available reference count increments available on
 * a PBN for a data_vio to deduplicate, a new lock is forked and the excess
 * waiters roll over to the new lock (which goes directly to WRITING). The new
 * lock takes the place of the old lock in the lock map so new data_vios will
 * be directed to it. The two locks will proceed independently, but only the
 * new lock will have the right to update the index (unless it also forks).
 *
 * Since rollover happens in a lock instance, once a valid data location has
 * been selected, it will not change. QUERYING and WRITING are only performed
 * once per lock lifetime. All other non-endpoint states can be re-entered.
 *
 * XXX still need doc on BYPASSING
 *
 * The function names in this module follow a convention referencing the
 * states and transitions in the state machine diagram for VDOSTORY-190.
 * [XXX link or repository path to it?]
 * For example, for the LOCKING state, there are start_locking() and
 * finish_locking() functions. start_locking() is invoked by the finish
 * function of the state (or states) that transition to LOCKING. It performs
 * the actual lock state change and must be invoked on the hash zone thread.
 * finish_locking() is called by (or continued via callback from) the code
 * actually obtaining the lock. It does any bookkeeping or decision-making
 * required and invokes the appropriate start function of the state being
 * transitioned to after LOCKING.
 **/

#include "hash-lock.h"

#include <linux/list.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "compression-state.h"
#include "constants.h"
#include "data-vio.h"
#include "hash-zone.h"
#include "packer.h"
#include "pbn-lock.h"
#include "physical-zone.h"
#include "slab.h"
#include "slab-depot.h"
#include "types.h"
#include "vdo.h"
#include "vio-write.h"
#include "wait-queue.h"

static const char *LOCK_STATE_NAMES[] = {
	[VDO_HASH_LOCK_BYPASSING] = "BYPASSING",
	[VDO_HASH_LOCK_DEDUPING] = "DEDUPING",
	[VDO_HASH_LOCK_DESTROYING] = "DESTROYING",
	[VDO_HASH_LOCK_INITIALIZING] = "INITIALIZING",
	[VDO_HASH_LOCK_LOCKING] = "LOCKING",
	[VDO_HASH_LOCK_QUERYING] = "QUERYING",
	[VDO_HASH_LOCK_UNLOCKING] = "UNLOCKING",
	[VDO_HASH_LOCK_UPDATING] = "UPDATING",
	[VDO_HASH_LOCK_VERIFYING] = "VERIFYING",
	[VDO_HASH_LOCK_WRITING] = "WRITING",
};

/* There are loops in the state diagram, so some forward decl's are needed. */
static void start_deduping(struct hash_lock *lock,
			   struct data_vio *agent,
			   bool agent_is_done);
static void start_locking(struct hash_lock *lock, struct data_vio *agent);
static void start_writing(struct hash_lock *lock, struct data_vio *agent);
static void unlock_duplicate_pbn(struct vdo_completion *completion);
static void transfer_allocation_lock(struct data_vio *data_vio);

/**
 * Get the PBN lock on the duplicate data location for a data_vio from the
 * hash_lock the data_vio holds (if there is one).
 *
 * @param data_vio  The data_vio to query
 *
 * @return The PBN lock on the data_vio's duplicate location
 **/
struct pbn_lock *vdo_get_duplicate_lock(struct data_vio *data_vio)
{
	if (data_vio->hash_lock == NULL) {
		return NULL;
	}
	return data_vio->hash_lock->duplicate_lock;
}

/**
 * Get the string representation of a hash lock state.
 *
 * @param state  The hash lock state
 *
 * @return The short string representing the state
 **/
const char *vdo_get_hash_lock_state_name(enum hash_lock_state state)
{
	/* Catch if a state has been added without updating the name array. */
	STATIC_ASSERT((VDO_HASH_LOCK_DESTROYING + 1)
		      == ARRAY_SIZE(LOCK_STATE_NAMES));
	return (state < ARRAY_SIZE(LOCK_STATE_NAMES)) ? LOCK_STATE_NAMES[state]
						      : NULL;
}

/**
 * Set the current state of a hash lock.
 *
 * @param lock       The lock to update
 * @param new_state  The new state
 **/
static void set_hash_lock_state(struct hash_lock *lock,
				enum hash_lock_state new_state)
{
	if (false) {
		uds_log_warning("XXX %px %s -> %s",
				(void *) lock,
				vdo_get_hash_lock_state_name(lock->state),
				vdo_get_hash_lock_state_name(new_state));
	}
	lock->state = new_state;
}

/**
 * Assert that a data_vio is the agent of its hash lock, and that this is being
 * called in the hash zone.
 *
 * @param data_vio  The data_vio expected to be the lock agent
 * @param where     A string describing the function making the assertion
 **/
static void assert_hash_lock_agent(struct data_vio *data_vio,
				   const char *where)
{
	/* Not safe to access the agent field except from the hash zone. */
	assert_data_vio_in_hash_zone(data_vio);
	ASSERT_LOG_ONLY(data_vio == data_vio->hash_lock->agent,
			"%s must be for the hash lock agent", where);
}

/**
 * Set or clear the lock agent.
 *
 * @param lock       The hash lock to update
 * @param new_agent  The new lock agent (may be NULL to clear the agent)
 **/
static void set_agent(struct hash_lock *lock, struct data_vio *new_agent)
{
	lock->agent = new_agent;
}

/**
 * Set the duplicate lock held by a hash lock. May only be called in the
 * physical zone of the PBN lock.
 *
 * @param hash_lock  The hash lock to update
 * @param pbn_lock   The PBN read lock to use as the duplicate lock
 **/
static void set_duplicate_lock(struct hash_lock *hash_lock,
			       struct pbn_lock *pbn_lock)
{
	ASSERT_LOG_ONLY((hash_lock->duplicate_lock == NULL),
			"hash lock must not already hold a duplicate lock");

	pbn_lock->holder_count += 1;
	hash_lock->duplicate_lock = pbn_lock;
}

/**
 * Convert a pointer to the hash_lock_entry field in a data_vio to the
 * enclosing data_vio.
 *
 * @param entry The list entry to convert
 *
 * @return A pointer to the data_vio containing the list entry
 **/
static inline struct data_vio *
data_vio_from_lock_entry(struct list_head *entry)
{
	return list_entry(entry, struct data_vio, hash_lock_entry);
}

/**
 * Remove the first data_vio from the lock's wait queue and return it.
 *
 * @param lock  The lock containing the wait queue
 *
 * @return The first (oldest) waiter in the queue, or <code>NULL</code> if
 *         the queue is empty
 **/
static inline struct data_vio *dequeue_lock_waiter(struct hash_lock *lock)
{
	return waiter_as_data_vio(dequeue_next_waiter(&lock->waiters));
}

/**
 * Continue processing a data_vio that has been waiting for an event, setting
 * the result from the event, and continuing in a specified callback function.
 *
 * @param data_vio   The data_vio to continue
 * @param result     The current result (will not mask older errors)
 * @param callback   The function in which to continue processing
 **/
static void continue_data_vio_in(struct data_vio *data_vio,
				 int result,
				 vdo_action *callback)
{
	data_vio_as_completion(data_vio)->callback = callback;
	continue_data_vio(data_vio, result);
}

/**
 * Set, change, or clear the hash lock a data_vio is using. Updates the hash
 * lock (or locks) to reflect the change in membership.
 *
 * @param data_vio  The data_vio to update
 * @param new_lock  The hash lock the data_vio is joining
 **/
static void set_hash_lock(struct data_vio *data_vio,
			  struct hash_lock *new_lock)
{
	struct hash_lock *old_lock = data_vio->hash_lock;

	if (old_lock != NULL) {
		ASSERT_LOG_ONLY(
			data_vio->hash_zone != NULL,
			"must have a hash zone when halding a hash lock");
		ASSERT_LOG_ONLY(
			!list_empty(&data_vio->hash_lock_entry),
			"must be on a hash lock ring when holding a hash lock");
		ASSERT_LOG_ONLY(old_lock->reference_count > 0,
				"hash lock reference must be counted");

		if ((old_lock->state != VDO_HASH_LOCK_BYPASSING)
		    && (old_lock->state != VDO_HASH_LOCK_UNLOCKING)) {
			/*
			 * If the reference count goes to zero in a non-
			 * terminal state, we're most likely leaking this lock.
			 */
			ASSERT_LOG_ONLY(
				old_lock->reference_count > 1,
				"hash locks should only become unreferenced in a terminal state, not state %s",
				vdo_get_hash_lock_state_name(old_lock->state));
		}

		list_del_init(&data_vio->hash_lock_entry);
		old_lock->reference_count -= 1;

		data_vio->hash_lock = NULL;
	}

	if (new_lock != NULL) {
		/*
		 * Keep all data_vios sharing the lock on a ring since they can
		 * complete in any order and we'll always need a pointer to one
		 * to compare data.
		 */
		list_move_tail(&data_vio->hash_lock_entry,
			       &new_lock->duplicate_ring);
		new_lock->reference_count += 1;

		/*
		 * XXX Not needed for VDOSTORY-190, but useful for checking
		 * whether a test is getting concurrent dedupe, and how much.
		 */
		if (new_lock->max_references < new_lock->reference_count) {
			new_lock->max_references = new_lock->reference_count;
		}

		data_vio->hash_lock = new_lock;
	}
}

/**
 * Bottleneck for data_vios that have written or deduplicated and that are no
 * longer needed to be an agent for the hash lock.
 *
 * @param data_vio  The data_vio to complete and send to be cleaned up
 **/
static void exit_hash_lock(struct data_vio *data_vio)
{
	/* Release the hash lock now, saving a thread transition in cleanup. */
	vdo_release_hash_lock(data_vio);

	/*
	 * Complete the data_vio and start the clean-up path in vioWrite to
	 * release any locks it still holds.
	 */
	finish_data_vio(data_vio, VDO_SUCCESS);
}

/**
 * Retire the active lock agent, replacing it with the first lock waiter, and
 * make the retired agent exit the hash lock.
 *
 * @param lock  The hash lock to update
 *
 * @return The new lock agent (which will be NULL if there was no waiter)
 **/
static struct data_vio *retire_lock_agent(struct hash_lock *lock)
{
	struct data_vio *old_agent = lock->agent;
	struct data_vio *new_agent = dequeue_lock_waiter(lock);

	set_agent(lock, new_agent);
	exit_hash_lock(old_agent);
	if (new_agent != NULL) {
		set_data_vio_duplicate_location(new_agent, lock->duplicate);
	}
	return new_agent;
}

/**
 * Callback to call launch_compress_data_vio(), putting a data_vio back on the
 * write path.
 *
 * @param completion  The data_vio
 **/
static void compress_data_callback(struct vdo_completion *completion)
{
	/*
	 * XXX VDOSTORY-190 need an error check since launch_compress_data_vio
	 * doesn't have one.
	 */
	launch_compress_data_vio(as_data_vio(completion));
}

/**
 * Add a data_vio to the lock's queue of waiters.
 *
 * @param lock      The hash lock on which to wait
 * @param data_vio  The data_vio to add to the queue
 **/
static void wait_on_hash_lock(struct hash_lock *lock,
			      struct data_vio *data_vio)
{
	int result = enqueue_data_vio(&lock->waiters, data_vio);
	if (result != VDO_SUCCESS) {
		/*
		 * This should be impossible, but if it somehow happens, give
		 * up on trying to dedupe the data.
		 */
		set_hash_lock(data_vio, NULL);
		continue_data_vio_in(data_vio, result, compress_data_callback);
		return;
	}

	/*
	 * Make sure the agent doesn't block indefinitely in the packer since
	 * it now has at least one other data_vio waiting on it.
	 */
	if ((lock->state == VDO_HASH_LOCK_WRITING)
	    && cancel_vio_compression(lock->agent)) {
		/*
		 * Even though we're waiting, we also have to send ourselves
		 * as a one-way message to the packer to ensure the agent
		 * continues executing. This is safe because
		 * cancel_vio_compression() guarantees the agent won't
		 * continue executing until this message arrives in the
		 * packer, and because the wait queue link isn't used for
		 * sending the message.
		 */
		data_vio->compression.lock_holder = lock->agent;
		launch_data_vio_packer_callback(data_vio,
						vdo_remove_lock_holder_from_packer);
	}
}

/**
 * waiter_callback function that calls launch_compress_data_vio on the
 * data_vio waiter.
 *
 * @param waiter   The data_vio's waiter link
 * @param context  Not used
 **/
static void compress_waiter(struct waiter *waiter,
			    void *context __always_unused)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);

	data_vio->is_duplicate = false;
	launch_compress_data_vio(data_vio);
}

/**
 * Handle the result of the agent for the lock releasing a read lock on
 * duplicate candidate due to aborting the hash lock. This continuation is
 * registered in unlock_duplicate_pbn().
 *
 * @param completion  The completion of the  acting as the lock's agent
 **/
static void finish_bypassing(struct vdo_completion *completion)
{
	struct data_vio *agent = as_data_vio(completion);
	struct hash_lock *lock = agent->hash_lock;

	assert_hash_lock_agent(agent, __func__);

	ASSERT_LOG_ONLY(lock->duplicate_lock == NULL,
			"must have released the duplicate lock for the hash lock");
	exit_hash_lock(agent);
}

/**
 * Stop using the hash lock, resuming the old write path for the lock agent
 * and any data_vios waiting on it, and put it in a state where data_vios
 * entering the lock will use the old dedupe path instead of waiting.
 *
 * @param lock   The hash lock
 * @param agent  The data_vio acting as the agent for the lock
 **/
static void start_bypassing(struct hash_lock *lock, struct data_vio *agent)
{
	set_hash_lock_state(lock, VDO_HASH_LOCK_BYPASSING);

	/* Ensure we don't attempt to update advice when cleaning up. */
	lock->update_advice = false;

	ASSERT_LOG_ONLY(((agent != NULL) || !has_waiters(&lock->waiters)),
			"should not have waiters without an agent");
	notify_all_waiters(&lock->waiters, compress_waiter, NULL);

	if (lock->duplicate_lock != NULL) {
		if (agent != NULL) {
			/*
			 * The agent must reference the duplicate zone to
			 * launch it.
			 */
			agent->duplicate = lock->duplicate;
			launch_data_vio_duplicate_zone_callback(
				agent,
				unlock_duplicate_pbn);
			return;
		}
		ASSERT_LOG_ONLY(
			false,
			"hash lock holding a PBN lock must have an agent");
	}

	if (agent == NULL) {
		return;
	}

	set_agent(lock, NULL);
	agent->is_duplicate = false;
	launch_compress_data_vio(agent);
}

/**
 * Abort processing on this hash lock when noticing an error. Currently, this
 * moves the hash lock to the BYPASSING state, to release all pending
 * data_vios.
 *
 * @param lock      The hash_lock
 * @param data_vio  The data_vio with the error
 **/
static void abort_hash_lock(struct hash_lock *lock, struct data_vio *data_vio)
{
	/*
	 * If we've already aborted the lock, don't try to re-abort it; just
	 * exit.
	 */
	if (lock->state == VDO_HASH_LOCK_BYPASSING) {
		exit_hash_lock(data_vio);
		return;
	}

	if (data_vio != lock->agent) {
		if ((lock->agent != NULL) || (lock->reference_count > 1)) {
			/*
			 * Other data_vios are still sharing the lock (which
			 * should be DEDUPING), so just kick this one out of
			 * the lock to report its error.
			 */
			ASSERT_LOG_ONLY(
				lock->agent == NULL,
				"only active agent should call abort_hash_lock");
			exit_hash_lock(data_vio);
			return;
		}
		/*
		 * Make the lone data_vio the lock agent so it can abort and
		 * clean up.
		 */
		set_agent(lock, data_vio);
	}

	start_bypassing(lock, data_vio);
}

/**
 * Handle the result of the agent for the lock releasing a read lock on
 * duplicate candidate. This continuation is registered in
 * unlock_duplicate_pbn().
 *
 * @param completion  The completion of the data_vio acting as the lock's agent
 **/
static void finish_unlocking(struct vdo_completion *completion)
{
	struct data_vio *agent = as_data_vio(completion);
	struct hash_lock *lock = agent->hash_lock;

	assert_hash_lock_agent(agent, __func__);

	ASSERT_LOG_ONLY(
		lock->duplicate_lock == NULL,
		"must have released the duplicate lock for the hash lock");

	if (completion->result != VDO_SUCCESS) {
		abort_hash_lock(lock, agent);
		return;
	}

	if (!lock->verified) {
		/*
		 * UNLOCKING -> WRITING transition: The lock we released was on
		 * an unverified block, so it must have been a lock on advice
		 * we were verifying, not on a location that was used for
		 * deduplication. Go write (or compress) the block to get a
		 * location to dedupe against.
		 */
		start_writing(lock, agent);
		return;
	}

	/*
	 * With the lock released, the verified duplicate block may already
	 * have changed and will need to be re-verified if a waiter arrived.
	 */
	lock->verified = false;

	if (has_waiters(&lock->waiters)) {
		/*
		 * UNLOCKING -> LOCKING transition: A new data_vio entered the
		 * hash lock while the agent was releasing the PBN lock. The
		 * current agent exits and the waiter has to re-lock and
		 * re-verify the duplicate location.
		 *
		 * XXX VDOSTORY-190 If we used the current agent to re-acquire
		 * the PBN lock we wouldn't need to re-verify.
		 */
		agent = retire_lock_agent(lock);
		start_locking(lock, agent);
		return;
	}

	/*
	 * UNLOCKING -> DESTROYING transition: The agent is done with the lock
	 * and no other data_vios reference it, so remove it from the lock map
	 * and return it to the pool.
	 */
	exit_hash_lock(agent);
}

/**
 * Release a read lock on the PBN of the block that may or may not have
 * contained duplicate data. This continuation is launched by
 * start_unlocking(), and calls back to finish_unlocking() on the hash zone
 * thread.
 *
 * @param completion  The completion of the data_vio acting as the lock's agent
 **/
static void unlock_duplicate_pbn(struct vdo_completion *completion)
{
	struct data_vio *agent = as_data_vio(completion);
	struct hash_lock *lock = agent->hash_lock;

	assert_data_vio_in_duplicate_zone(agent);

	ASSERT_LOG_ONLY(lock->duplicate_lock != NULL,
			"must have a duplicate lock to release");

	vdo_release_physical_zone_pbn_lock(agent->duplicate.zone,
					   agent->duplicate.pbn,
					   UDS_FORGET(lock->duplicate_lock));

	if (lock->state == VDO_HASH_LOCK_BYPASSING) {
		launch_data_vio_hash_zone_callback(agent,
						   finish_bypassing);
	} else {
		launch_data_vio_hash_zone_callback(agent, finish_unlocking);
	}
}

/**
 * Release a read lock on the PBN of the block that may or may not have
 * contained duplicate data.
 *
 * @param lock   The hash lock
 * @param agent  The data_vio currently acting as the agent for the lock
 **/
static void start_unlocking(struct hash_lock *lock, struct data_vio *agent)
{
	set_hash_lock_state(lock, VDO_HASH_LOCK_UNLOCKING);

	launch_data_vio_duplicate_zone_callback(agent, unlock_duplicate_pbn);
}

/**
 * Process the result of a UDS update performed by the agent for the lock.
 * This continuation is registered in start_querying().
 *
 * @param completion  The completion of the data_vio that performed the update
 **/
static void finish_updating(struct vdo_completion *completion)
{
	struct data_vio *agent = as_data_vio(completion);
	struct hash_lock *lock = agent->hash_lock;

	assert_hash_lock_agent(agent, __func__);

	if (completion->result != VDO_SUCCESS) {
		abort_hash_lock(lock, agent);
		return;
	}

	/*
	 * UDS was updated successfully, so don't update again unless the
	 * duplicate location changes due to rollover.
	 */
	lock->update_advice = false;

	if (has_waiters(&lock->waiters)) {
		/*
		 * UPDATING -> DEDUPING transition: A new data_vio arrived
		 * during the UDS update. Send it on the verified dedupe path.
		 * The agent is done with the lock, but the lock may still need
		 * to use it to clean up after rollover.
		 */
		start_deduping(lock, agent, true);
		return;
	}

	if (lock->duplicate_lock != NULL) {
		/*
		 * UPDATING -> UNLOCKING transition: No one is waiting to
		 * dedupe, but we hold a duplicate PBN lock, so go release it.
		 */
		start_unlocking(lock, agent);
	} else {
		/*
		 * UPDATING -> DESTROYING transition: No one is waiting to
		 * dedupe and there's no lock to release.
		 *
		 * XXX startDestroying(lock, agent);
		 */
		start_bypassing(lock, NULL);
		exit_hash_lock(agent);
	}
}

/**
 * Continue deduplication with the last step, updating UDS with the location
 * of the duplicate that should be returned as advice in the future.
 *
 * @param lock   The hash lock
 * @param agent  The data_vio currently acting as the agent for the lock
 **/
static void start_updating(struct hash_lock *lock, struct data_vio *agent)
{
	set_hash_lock_state(lock, VDO_HASH_LOCK_UPDATING);

	ASSERT_LOG_ONLY(lock->verified,
			"new advice should have been verified");
	ASSERT_LOG_ONLY(lock->update_advice,
			"should only update advice if needed");

	agent->last_async_operation = VIO_ASYNC_OP_UPDATE_DEDUPE_INDEX;
	set_data_vio_hash_zone_callback(agent, finish_updating);
	vdo_update_dedupe_index(agent);
}

/**
 * Handle a data_vio that has finished deduplicating against the block locked
 * by the hash lock. If there are other data_vios still sharing the lock, this
 * will just release the data_vio's share of the lock and finish processing the
 * data_vio. If this is the last data_vio holding the lock, this makes the
 * data_vio the lock agent and uses it to advance the state of the lock so it
 * can eventually be released.
 *
 * @param lock      The hash lock
 * @param data_vio  The lock holder that has finished deduplicating
 **/
static void finish_deduping(struct hash_lock *lock, struct data_vio *data_vio)
{
	struct data_vio *agent = data_vio;

	ASSERT_LOG_ONLY(lock->agent == NULL,
			"shouldn't have an agent in DEDUPING");
	ASSERT_LOG_ONLY(!has_waiters(&lock->waiters),
			"shouldn't have any lock waiters in DEDUPING");

	/*
	 * Just release the lock reference if other data_vios are still
	 * deduping.
	 */
	if (lock->reference_count > 1) {
		exit_hash_lock(data_vio);
		return;
	}

	/* The hash lock must have an agent for all other lock states. */
	set_agent(lock, agent);

	if (lock->update_advice) {
		/*
		 * DEDUPING -> UPDATING transition: The location of the
		 * duplicate block changed since the initial UDS query because
		 * of compression, rollover, or because the query agent didn't
		 * have an allocation. The UDS update was delayed in case there
		 * was another change in location, but with only this data_vio
		 * using the hash lock, it's time to update the advice.
		 */
		start_updating(lock, agent);
	} else {
		/*
		 * DEDUPING -> UNLOCKING transition: Release the PBN read lock
		 * on the duplicate location so the hash lock itself can be
		 * released (contingent on no new data_vios arriving in the
		 * lock before the agent returns).
		 */
		start_unlocking(lock, agent);
	}
}

/**
 * Implements waiter_callback. Binds the data_vio that was waiting to a new
 * hash lock and waits on that lock.
 **/
static void enter_forked_lock(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct hash_lock *new_lock = (struct hash_lock *) context;

	set_hash_lock(data_vio, new_lock);
	wait_on_hash_lock(new_lock, data_vio);
}

/**
 * Fork a hash lock because it has run out of increments on the duplicate PBN.
 * Transfers the new agent and any lock waiters to a new hash lock instance
 * which takes the place of the old lock in the lock map. The old lock remains
 * active, but will not update advice.
 *
 * @param old_lock   The hash lock to fork
 * @param new_agent  The data_vio that will be the agent for the new lock
 **/
static void fork_hash_lock(struct hash_lock *old_lock,
			   struct data_vio *new_agent)
{
	struct hash_lock *new_lock;
	int result = vdo_acquire_lock_from_hash_zone(new_agent->hash_zone,
						     &new_agent->chunk_name,
						     old_lock, &new_lock);
	if (result != VDO_SUCCESS) {
		abort_hash_lock(old_lock, new_agent);
		return;
	}

	/*
	 * Only one of the two locks should update UDS. The old lock is out of
	 * references, so it would be poor dedupe advice in the short term.
	 */
	old_lock->update_advice = false;
	new_lock->update_advice = true;

	set_hash_lock(new_agent, new_lock);
	set_agent(new_lock, new_agent);

	notify_all_waiters(&old_lock->waiters, enter_forked_lock, new_lock);

	new_agent->is_duplicate = false;
	start_writing(new_lock, new_agent);
}

/**
 * Reserve a reference count increment for a data_vio and launch it on the
 * dedupe path. If no increments are available, this will roll over to a new
 * hash lock and launch the data_vio as the writing agent for that lock.
 *
 * @param lock       The hash lock
 * @param data_vio   The data_vio to deduplicate using the hash lock
 * @param has_claim  <code>true</code> if the data_vio already has claimed
 *                   an increment from the duplicate lock
 **/
static void launch_dedupe(struct hash_lock *lock,
			  struct data_vio *data_vio,
			  bool has_claim)
{
	if (!has_claim &&
	    !vdo_claim_pbn_lock_increment(lock->duplicate_lock)) {
		/* Out of increments, so must roll over to a new lock. */
		fork_hash_lock(lock, data_vio);
		return;
	}

	/* Deduplicate against the lock's verified location. */
	set_data_vio_duplicate_location(data_vio, lock->duplicate);
	launch_deduplicate_data_vio(data_vio);
}

/**
 * Enter the hash lock state where data_vios deduplicate in parallel against a
 * true copy of their data on disk. If the agent itself needs to deduplicate,
 * an increment for it must already have been claimed from the duplicate lock,
 * ensuring the hash lock will still have a data_vio holding it.
 *
 * @param lock           The hash lock
 * @param agent          The data_vio acting as the agent for the lock
 * @param agent_is_done  <code>true</code> only if the agent has already
 *                       written or deduplicated against its data
 **/
static void start_deduping(struct hash_lock *lock,
			   struct data_vio *agent,
			   bool agent_is_done)
{
	set_hash_lock_state(lock, VDO_HASH_LOCK_DEDUPING);

	/*
	 * We don't take the downgraded allocation lock from the agent unless
	 * we actually need to deduplicate against it.
	 */
	if (lock->duplicate_lock == NULL) {
		ASSERT_LOG_ONLY(
			!vdo_is_state_compressed(agent->new_mapped.state),
			"compression must have shared a lock");
		ASSERT_LOG_ONLY(agent_is_done,
				"agent must have written the new duplicate");
		transfer_allocation_lock(agent);
	}

	ASSERT_LOG_ONLY(vdo_is_pbn_read_lock(lock->duplicate_lock),
			"duplicate_lock must be a PBN read lock");

	/*
	 * This state is not like any of the other states. There is no
	 * designated agent--the agent transitioning to this state and all the
	 * waiters will be launched to deduplicate in parallel.
	 */
	set_agent(lock, NULL);

	/*
	 * Launch the agent (if not already deduplicated) and as many lock
	 * waiters as we have available increments for on the dedupe path. If
	 * we run out of increments, rollover will be triggered and the
	 * remaining waiters will be transferred to the new lock.
	 */
	if (!agent_is_done) {
		launch_dedupe(lock, agent, true);
		agent = NULL;
	}
	while (has_waiters(&lock->waiters)) {
		launch_dedupe(lock, dequeue_lock_waiter(lock), false);
	}

	if (agent_is_done) {
		/*
		 * In the degenerate case where all the waiters rolled over to
		 * a new lock, this will continue to use the old agent to
		 * clean up this lock, and otherwise it just lets the agent
		 * exit the lock.
		 */
		finish_deduping(lock, agent);
	}
}

/**
 * Handle the result of the agent for the lock comparing its data to the
 * duplicate candidate. This continuation is registered in start_verifying().
 *
 * @param completion  The completion of the data_vio used to verify dedupe
 **/
static void finish_verifying(struct vdo_completion *completion)
{
	struct data_vio *agent = as_data_vio(completion);
	struct hash_lock *lock = agent->hash_lock;

	assert_hash_lock_agent(agent, __func__);

	if (completion->result != VDO_SUCCESS) {
		/*
		 * XXX VDOSTORY-190 should convert verify IO errors to
		 * verification failure
		 */
		abort_hash_lock(lock, agent);
		return;
	}

	lock->verified = agent->is_duplicate;

	/*
	 * Only count the result of the initial verification of the advice as
	 * valid or stale, and not any re-verifications due to PBN lock
	 * releases.
	 */
	if (!lock->verify_counted) {
		lock->verify_counted = true;
		if (lock->verified) {
			vdo_bump_hash_zone_valid_advice_count(agent->hash_zone);
		} else {
			vdo_bump_hash_zone_stale_advice_count(agent->hash_zone);
		}
	}

	/*
	 * Even if the block is a verified duplicate, we can't start to
	 * deduplicate unless we can claim a reference count increment for the
	 * agent.
	 */
	if (lock->verified &&
	    !vdo_claim_pbn_lock_increment(lock->duplicate_lock)) {
		agent->is_duplicate = false;
		lock->verified = false;
	}

	if (lock->verified) {
		/*
		 * VERIFYING -> DEDUPING transition: The advice is for a true
		 * duplicate, so start deduplicating against it, if references
		 * are available.
		 */
		start_deduping(lock, agent, false);
	} else {
		/*
		 * VERIFYING -> UNLOCKING transition: Either the verify failed
		 * or we'd try to dedupe and roll over immediately, which would
		 * fail because it would leave the lock without an agent to
		 * release the PBN lock. In both cases, the data will have to
		 * be written or compressed, but first the advice PBN must be
		 * unlocked by the VERIFYING agent.
		 */
		lock->update_advice = true;
		start_unlocking(lock, agent);
	}
}

/**
 * Continue the deduplication path for a hash lock by using the agent to read
 * (and possibly decompress) the data at the candidate duplicate location,
 * comparing it to the data in the agent to verify that the candidate is
 * identical to all the data_vios sharing the hash. If so, it can be
 * deduplicated against, otherwise a data_vio allocation will have to be
 * written to and used for dedupe.
 *
 * @param lock   The hash lock (must be LOCKING)
 * @param agent  The data_vio to use to read and compare candidate data
 **/
static void start_verifying(struct hash_lock *lock, struct data_vio *agent)
{
	set_hash_lock_state(lock, VDO_HASH_LOCK_VERIFYING);
	ASSERT_LOG_ONLY(!lock->verified,
			"hash lock only verifies advice once");

	agent->last_async_operation = VIO_ASYNC_OP_VERIFY_DUPLICATION;
	set_data_vio_hash_zone_callback(agent, finish_verifying);
	verify_data_vio_duplication(agent);
}

/**
 * Handle the result of the agent for the lock attempting to obtain a PBN read
 * lock on the candidate duplicate block. this continuation is registered in
 * lock_duplicate_pbn().
 *
 * @param completion  The completion of the data_vio that attempted to get
 *                    the read lock
 **/
static void finish_locking(struct vdo_completion *completion)
{
	struct data_vio *agent = as_data_vio(completion);
	struct hash_lock *lock = agent->hash_lock;

	assert_hash_lock_agent(agent, __func__);

	if (completion->result != VDO_SUCCESS) {
		/* XXX clearDuplicateLocation()? */
		agent->is_duplicate = false;
		abort_hash_lock(lock, agent);
		return;
	}

	if (!agent->is_duplicate) {
		ASSERT_LOG_ONLY(
			lock->duplicate_lock == NULL,
			"must not hold duplicate_lock if not flagged as a duplicate");
		/*
		 * LOCKING -> WRITING transition: The advice block is being
		 * modified or has no available references, so try to write or
		 * compress the data, remembering to update UDS later with the
		 * new advice.
		 */
		vdo_bump_hash_zone_stale_advice_count(agent->hash_zone);
		lock->update_advice = true;
		start_writing(lock, agent);
		return;
	}

	ASSERT_LOG_ONLY(lock->duplicate_lock != NULL,
			"must hold duplicate_lock if flagged as a duplicate");

	if (!lock->verified) {
		/*
		 * LOCKING -> VERIFYING transition: Continue on the unverified
		 * dedupe path, reading the candidate duplicate and comparing
		 * it to the agent's data to decide whether it is a true
		 * duplicate or stale advice.
		 */
		start_verifying(lock, agent);
		return;
	}

	if (!vdo_claim_pbn_lock_increment(lock->duplicate_lock)) {
		/*
		 * LOCKING -> UNLOCKING transition: The verified block was
		 * re-locked, but has no available increments left. Must first
		 * release the useless PBN read lock before rolling over to a
		 * new copy of the block.
		 */
		agent->is_duplicate = false;
		lock->verified = false;
		lock->update_advice = true;
		start_unlocking(lock, agent);
		return;
	}

	/*
	 * LOCKING -> DEDUPING transition: Continue on the verified dedupe
	 * path, deduplicating against a location that was previously verified
	 * or written to.
	 */
	start_deduping(lock, agent, false);
}

/**
 * Acquire a read lock on the PBN of the block containing candidate duplicate
 * data (compressed or uncompressed). If the PBN is already locked for
 * writing, the lock attempt is abandoned and is_duplicate will be cleared
 * before calling back. this continuation is launched from start_locking(), and
 * calls back to finish_locking() on the hash zone thread.
 *
 * @param completion The completion of the data_vio attempting to acquire the
 *                   physical block lock on behalf of its hash lock
 **/
static void lock_duplicate_pbn(struct vdo_completion *completion)
{
	unsigned int increment_limit;
	struct pbn_lock *lock;
	int result;

	struct data_vio *agent = as_data_vio(completion);
	struct slab_depot *depot = vdo_get_from_data_vio(agent)->depot;
	struct physical_zone *zone = agent->duplicate.zone;

	assert_data_vio_in_duplicate_zone(agent);

	set_data_vio_hash_zone_callback(agent, finish_locking);

	/*
	 * While in the zone that owns it, find out how many additional
	 * references can be made to the block if it turns out to truly be a
	 * duplicate.
	 */
	increment_limit = vdo_get_increment_limit(depot, agent->duplicate.pbn);
	if (increment_limit == 0) {
		/*
		 * We could deduplicate against it later if a reference
		 * happened to be released during verification, but it's
		 * probably better to bail out now.
		 * XXX clearDuplicateLocation()?
		 */
		agent->is_duplicate = false;
		continue_data_vio(agent, VDO_SUCCESS);
		return;
	}

	result = vdo_attempt_physical_zone_pbn_lock(zone, agent->duplicate.pbn,
						    VIO_READ_LOCK, &lock);
	if (result != VDO_SUCCESS) {
		continue_data_vio(agent, result);
		return;
	}

	if (!vdo_is_pbn_read_lock(lock)) {
		/*
		 * There are three cases of write locks: uncompressed data
		 * block writes, compressed (packed) block writes, and block
		 * map page writes. In all three cases, we give up on trying
		 * to verify the advice and don't bother to try deduplicate
		 * against the data in the write lock holder.
		 *
		 * 1) We don't ever want to try to deduplicate against a block
		 * map page.
		 *
		 * 2a) It's very unlikely we'd deduplicate against an entire
		 * packed block, both because of the chance of matching it,
		 * and because we don't record advice for it, but for the
		 * uncompressed representation of all the fragments it
		 * contains. The only way we'd be getting lock contention is
		 * if we've written the same representation coincidentally
		 * before, had it become unreferenced, and it just happened to
		 * be packed together from compressed writes when we go to
		 * verify the lucky advice. Giving up is a miniscule loss of
		 * potential dedupe.
		 *
		 * 2b) If the advice is for a slot of a compressed block, it's
		 * about to get smashed, and the write smashing it cannot
		 * contain our data--it would have to be writing on behalf of
		 * our hash lock, but that's impossible since we're the lock
		 * agent.
		 *
		 * 3a) If the lock is held by a data_vio with different data,
		 * the advice is already stale or is about to become stale.
		 *
		 * 3b) If the lock is held by a data_vio that matches us, we
		 * may as well either write it ourselves (or reference the
		 * copy we already wrote) instead of potentially having many
		 * duplicates wait for the lock holder to write, journal,
		 * hash, and finally arrive in the hash lock. We lose a
		 * chance to avoid a UDS update in the very rare case of
		 * advice for a free block that just happened to be allocated
		 * to a data_vio with the same hash. There's also a chance to
		 * save on a block write, at the cost of a block verify.
		 * Saving on a full block compare in all stale advice cases
		 * almost certainly outweighs saving a UDS update and trading
		 * a write for a read in a lucky case where advice would have
		 * been saved from becoming stale.
		 * XXX clearDuplicateLocation()?
		 */
		agent->is_duplicate = false;
		continue_data_vio(agent, VDO_SUCCESS);
		return;
	}

	if (lock->holder_count == 0) {
		/* Ensure that the newly-locked block is referenced. */
		struct vdo_slab *slab =
			vdo_get_slab(depot, agent->duplicate.pbn);

		result = vdo_acquire_provisional_reference(
				slab, agent->duplicate.pbn, lock);
		if (result != VDO_SUCCESS) {
			uds_log_warning_strerror(result,
						 "Error acquiring provisional reference for dedupe candidate; aborting dedupe");
			agent->is_duplicate = false;
			vdo_release_physical_zone_pbn_lock(
				zone, agent->duplicate.pbn, UDS_FORGET(lock));
			continue_data_vio(agent, result);
			return;
		}

		/*
		 * The increment limit we grabbed earlier is still valid. The
		 * lock now holds the rights to acquire all those references.
		 * Those rights will be claimed by hash locks sharing this read
		 * lock.
		 */
		lock->increment_limit = increment_limit;
	}

	/*
	 * We've successfully acquired a read lock on behalf of the hash lock,
	 * so mark it as such.
	 */
	set_duplicate_lock(agent->hash_lock, lock);

	/*
	 * XXX VDOSTORY-190 Optimization: Same as start_locking() lazily
	 * changing state to save on having to switch back to the hash zone
	 * thread. Here we could directly launch the block verify, then switch
	 * to a hash thread.
	 */
	continue_data_vio(agent, VDO_SUCCESS);
}

/**
 * Continue deduplication for a hash lock that has obtained valid advice
 * of a potential duplicate through its agent.
 *
 * @param lock   The hash lock (currently must be QUERYING)
 * @param agent  The data_vio bearing the dedupe advice
 **/
static void start_locking(struct hash_lock *lock, struct data_vio *agent)
{
	ASSERT_LOG_ONLY(
		lock->duplicate_lock == NULL,
		"must not acquire a duplicate lock when already holding it");

	set_hash_lock_state(lock, VDO_HASH_LOCK_LOCKING);

	/*
	 * XXX VDOSTORY-190 Optimization: If we arrange to continue on the
	 * duplicate zone thread when accepting the advice, and don't
	 * explicitly change lock states (or use an agent-local state, or an
	 * atomic), we can avoid a thread transition here.
	 */
	agent->last_async_operation = VIO_ASYNC_OP_LOCK_DUPLICATE_PBN;
	launch_data_vio_duplicate_zone_callback(agent, lock_duplicate_pbn);
}

/**
 * Re-entry point for the lock agent after it has finished writing or
 * compressing its copy of the data block. The agent will never need to dedupe
 * against anything, so it's done with the lock, but the lock may not be
 * finished with it, as a UDS update might still be needed.
 *
 * If there are other lock holders, the agent will hand the job to one of them
 * and exit, leaving the lock to deduplicate against the just-written block.
 * If there are no other lock holders, the agent either exits (and later tears
 * down the hash lock), or it remains the agent and updates UDS.
 *
 * @param lock   The hash lock, which must be in state WRITING
 * @param agent  The data_vio that wrote its data for the lock
 **/
static void finish_writing(struct hash_lock *lock, struct data_vio *agent)
{
	/*
	 * Dedupe against the data block or compressed block slot the agent
	 * wrote. Since we know the write succeeded, there's no need to verify
	 * it.
	 */
	lock->duplicate = agent->new_mapped;
	lock->verified = true;

	if (vdo_is_state_compressed(lock->duplicate.state) &&
	    lock->registered) {
		/*
		 * Compression means the location we gave in the UDS query is
		 * not the location we're using to deduplicate.
		 */
		lock->update_advice = true;
	}

	/* If there are any waiters, we need to start deduping them. */
	if (has_waiters(&lock->waiters)) {
		/*
		 * WRITING -> DEDUPING transition: an asynchronously-written
		 * block failed to compress, so the PBN lock on the written
		 * copy was already transferred. The agent is done with the
		 * lock, but the lock may still need to use it to clean up
		 * after rollover.
		 */
		start_deduping(lock, agent, true);
		return;
	}

	/*
	 * There are no waiters and the agent has successfully written, so take
	 * a step towards being able to release the hash lock (or just release
	 * it).
	 */
	if (lock->update_advice) {
		/*
		 * WRITING -> UPDATING transition: There's no waiter and a UDS
		 * update is needed, so retain the WRITING agent and use it to
		 * launch the update. The happens on compression, rollover, or
		 * the QUERYING agent not having an allocation.
		 */
		start_updating(lock, agent);
	} else if (lock->duplicate_lock != NULL) {
		/*
		 * WRITING -> UNLOCKING transition: There's no waiter and no
		 * update needed, but the compressed write gave us a shared
		 * duplicate lock that we must release.
		 */
		set_data_vio_duplicate_location(agent, lock->duplicate);
		start_unlocking(lock, agent);
	} else {
		/*
		 * WRITING -> DESTROYING transition: There's no waiter, no
		 * update needed, and no duplicate lock held, so both the agent
		 * and lock have no more work to do. The agent will release its
		 * allocation lock in cleanup.
		 */
		/* XXX startDestroying(lock, agent); */
		start_bypassing(lock, NULL);
		exit_hash_lock(agent);
	}
}

/**
 * Search through the lock waiters for a data_vio that has an allocation. If
 * one is found, swap agents, put the old agent at the head of the wait queue,
 * then return the new agent. Otherwise, just return the current agent.
 *
 * @param lock   The hash lock to modify
 **/
static struct data_vio *select_writing_agent(struct hash_lock *lock)
{
	struct wait_queue temp_queue;
	int result;
	struct data_vio *data_vio;

	initialize_wait_queue(&temp_queue);

	/*
	 * This should-be-impossible condition is the only cause for
	 * enqueue_data_vio() to fail later on, where it would be a pain to
	 * handle.
	 */
	result = ASSERT(!is_waiting(data_vio_as_waiter(lock->agent)),
			"agent must not be waiting");
	if (result != VDO_SUCCESS) {
		return lock->agent;
	}

	/*
	 * Move waiters to the temp queue one-by-one until we find an
	 * allocation. Not ideal to search, but it only happens when nearly out
	 * of space.
	 */
	while (((data_vio = dequeue_lock_waiter(lock)) != NULL)
	       && !data_vio_has_allocation(data_vio)) {
		/*
		 * Use the lower-level enqueue since we're just moving waiters
		 * around.
		 */
		result = enqueue_waiter(&temp_queue,
					data_vio_as_waiter(data_vio));
		/*
		 * The only error is the data_vio already being on a wait
		 * queue. Since we just dequeued it, that could only happen
		 * due to a memory smash or concurrent use of that data_vio.
		 */
		ASSERT_LOG_ONLY(result == VDO_SUCCESS,
				"impossible enqueue_waiter error");
	}

	if (data_vio != NULL) {
		/*
		 * Move the rest of the waiters over to the temp queue,
		 * preserving the order they arrived at the lock.
		 */
		transfer_all_waiters(&lock->waiters, &temp_queue);

		/*
		 * The current agent is being replaced and will have to wait to
		 * dedupe; make it the first waiter since it was the first to
		 * reach the lock.
		 */
		result = enqueue_data_vio(&lock->waiters, lock->agent);
		ASSERT_LOG_ONLY(result == VDO_SUCCESS,
				"impossible enqueue_data_vio error after is_waiting checked");
		set_agent(lock, data_vio);
	} else {
		/* No one has an allocation, so keep the current agent. */
		data_vio = lock->agent;
	}

	/* Swap all the waiters back onto the lock's queue. */
	transfer_all_waiters(&temp_queue, &lock->waiters);
	return data_vio;
}

/**
 * Begin the non-duplicate write path for a hash lock that had no advice,
 * selecting a data_vio with an allocation as a new agent, if necessary,
 * then resuming the agent on the data_vio write path.
 *
 * @param lock   The hash lock (currently must be QUERYING)
 * @param agent  The data_vio currently acting as the agent for the lock
 **/
static void start_writing(struct hash_lock *lock, struct data_vio *agent)
{
	set_hash_lock_state(lock, VDO_HASH_LOCK_WRITING);

	/*
	 * The agent might not have received an allocation and so can't be used
	 * for writing, but it's entirely possible that one of the waiters did.
	 */
	if (!data_vio_has_allocation(agent)) {
		agent = select_writing_agent(lock);
		/*
		 * If none of the waiters had an allocation, the writes all
		 * have to fail.
		 */
		if (!data_vio_has_allocation(agent)) {
			/*
			 * XXX VDOSTORY-190 Should we keep a variant of
			 * BYPASSING that causes new arrivals to fail
			 * immediately if they don't have an allocation? It
			 * might be possible that on some path there would be
			 * non-waiters still referencing the lock, so it would
			 * remain in the map as everything is currently
			 * spelled, even if the agent and all waiters release.
			 */
			start_bypassing(lock, agent);
			return;
		}
	}

	/*
	 * If the agent compresses, it might wait indefinitely in the packer,
	 * which would be bad if there are any other data_vios waiting.
	 */
	if (has_waiters(&lock->waiters)) {
		cancel_vio_compression(agent);
	}

	/*
	 * Send the agent to the compress/pack/write path in vioWrite.  If it
	 * succeeds, it will return to the hash lock via
	 * vdo_continue_hash_lock() and call finish_writing().
	 */
	launch_compress_data_vio(agent);
}

/**
 * Process the result of a UDS query performed by the agent for the lock. This
 * continuation is registered in start_querying().
 *
 * @param completion  The completion of the data_vio that performed the query
 **/
static void finish_querying(struct vdo_completion *completion)
{
	struct data_vio *agent = as_data_vio(completion);
	struct hash_lock *lock = agent->hash_lock;

	assert_hash_lock_agent(agent, __func__);

	if (completion->result != VDO_SUCCESS) {
		abort_hash_lock(lock, agent);
		return;
	}

	if (agent->is_duplicate) {
		lock->duplicate = agent->duplicate;
		/*
		 * QUERYING -> LOCKING transition: Valid advice was obtained
		 * from UDS. Use the QUERYING agent to start the hash lock on
		 * the unverified dedupe path, verifying that the advice can be
		 * used.
		 */
		start_locking(lock, agent);
	} else {
		/*
		 * The agent will be used as the duplicate if has an
		 * allocation; if it does, that location was posted to UDS, so
		 * no update will be needed.
		 */
		lock->update_advice = !data_vio_has_allocation(agent);
		/*
		 * QUERYING -> WRITING transition: There was no advice or the
		 * advice wasn't valid, so try to write or compress the data.
		 */
		start_writing(lock, agent);
	}
}

/**
 * Start deduplication for a hash lock that has finished initializing by
 * making the data_vio that requested it the agent, entering the QUERYING
 * state, and using the agent to perform the UDS query on behalf of the lock.
 *
 * @param lock      The initialized hash lock
 * @param data_vio  The data_vio that has just obtained the new lock
 **/
static void start_querying(struct hash_lock *lock, struct data_vio *data_vio)
{
	set_agent(lock, data_vio);
	set_hash_lock_state(lock, VDO_HASH_LOCK_QUERYING);

	data_vio->last_async_operation = VIO_ASYNC_OP_CHECK_FOR_DUPLICATION;
	set_data_vio_hash_zone_callback(data_vio, finish_querying);
	check_data_vio_for_duplication(data_vio);
}

/**
 * Complain that a data_vio has entered a hash_lock that is in an unimplemented
 * or unusable state and continue the data_vio with an error.
 *
 * @param lock      The hash lock
 * @param data_vio  The data_vio attempting to enter the lock
 **/
static void report_bogus_lock_state(struct hash_lock *lock,
				    struct data_vio *data_vio)
{
	int result =
		ASSERT_FALSE("hash lock must not be in unimplemented state %s",
			     vdo_get_hash_lock_state_name(lock->state));
	continue_data_vio_in(data_vio, result, compress_data_callback);
}

/**
 * Asynchronously process a data_vio that has just acquired its reference to a
 * hash lock. This may place the data_vio on a wait queue, or it may use the
 * data_vio to perform operations on the lock's behalf.
 *
 * @param data_vio  The data_vio that has just acquired a lock on its chunk
 *                  name
 **/
void vdo_enter_hash_lock(struct data_vio *data_vio)
{
	struct hash_lock *lock = data_vio->hash_lock;

	switch (lock->state) {
	case VDO_HASH_LOCK_INITIALIZING:
		start_querying(lock, data_vio);
		break;

	case VDO_HASH_LOCK_QUERYING:
	case VDO_HASH_LOCK_WRITING:
	case VDO_HASH_LOCK_UPDATING:
	case VDO_HASH_LOCK_LOCKING:
	case VDO_HASH_LOCK_VERIFYING:
	case VDO_HASH_LOCK_UNLOCKING:
		/* The lock is busy, and can't be shared yet. */
		wait_on_hash_lock(lock, data_vio);
		break;

	case VDO_HASH_LOCK_BYPASSING:
		/* Bypass dedupe entirely. */
		launch_compress_data_vio(data_vio);
		break;

	case VDO_HASH_LOCK_DEDUPING:
		launch_dedupe(lock, data_vio, false);
		break;

	case VDO_HASH_LOCK_DESTROYING:
		/* A lock in this state should not be acquired by new VIOs. */
		report_bogus_lock_state(lock, data_vio);
		break;

	default:
		report_bogus_lock_state(lock, data_vio);
	}
}

/**
 * Asynchronously continue processing a data_vio in its hash lock after it has
 * finished writing, compressing, or deduplicating, so it can share the result
 * with any data_vios waiting in the hash lock, or update the UDS index, or
 * simply release its share of the lock. This must only be called in the
 * correct thread for the hash zone.
 *
 * @param data_vio  The data_vio to continue processing in its hash lock
 **/
void vdo_continue_hash_lock(struct data_vio *data_vio)
{
	struct hash_lock *lock = data_vio->hash_lock;
	/*
	 * XXX VDOSTORY-190 Eventually we may be able to fold the error
	 * handling in here instead of using a separate entry point for it.
	 */

	switch (lock->state) {
	case VDO_HASH_LOCK_WRITING:
		ASSERT_LOG_ONLY(data_vio == lock->agent,
				"only the lock agent may continue the lock");
		finish_writing(lock, data_vio);
		break;

	case VDO_HASH_LOCK_DEDUPING:
		finish_deduping(lock, data_vio);
		break;

	case VDO_HASH_LOCK_BYPASSING:
		/*
		 * This data_vio has finished the write path and the lock
		 * doesn't need it.
		 *
		 * XXX This isn't going to be correct if DEDUPING ever uses
		 * BYPASSING.
		 */
		finish_data_vio(data_vio, VDO_SUCCESS);
		break;

	case VDO_HASH_LOCK_INITIALIZING:
	case VDO_HASH_LOCK_QUERYING:
	case VDO_HASH_LOCK_UPDATING:
	case VDO_HASH_LOCK_LOCKING:
	case VDO_HASH_LOCK_VERIFYING:
	case VDO_HASH_LOCK_UNLOCKING:
	case VDO_HASH_LOCK_DESTROYING:
		/* A lock in this state should never be re-entered. */
		report_bogus_lock_state(lock, data_vio);
		break;

	default:
		report_bogus_lock_state(lock, data_vio);
	}
}

/**
 * Re-enter the hash lock after encountering an error, to clean up the hash
 * lock.
 *
 * @param data_vio  The data_vio with an error
 **/
void vdo_continue_hash_lock_on_error(struct data_vio *data_vio)
{
	/*
	 * XXX We could simply use vdo_continue_hash_lock() and check for
	 * errors in that.
	 */
	abort_hash_lock(data_vio->hash_lock, data_vio);
}

/**
 * Check whether the data in data_vios sharing a lock is different than in a
 * data_vio seeking to share the lock, which should only be possible in the
 * extremely unlikely case of a hash collision.
 *
 * @param lock       The lock to check
 * @param candidate  The data_vio seeking to share the lock
 *
 * @return <code>true</code> if the given data_vio must not share the lock
 *         because it doesn't have the same data as the lock holders
 **/
static bool is_hash_collision(struct hash_lock *lock,
			      struct data_vio *candidate)
{
	struct data_vio *lock_holder;
	bool collides;

	if (list_empty(&lock->duplicate_ring)) {
		return false;
	}

	lock_holder = data_vio_from_lock_entry(lock->duplicate_ring.next);
	collides = !compare_data_vios(lock_holder, candidate);

	if (collides) {
		vdo_bump_hash_zone_collision_count(candidate->hash_zone);
	} else {
		vdo_bump_hash_zone_data_match_count(candidate->hash_zone);
	}

	return collides;
}

static inline int
assert_hash_lock_preconditions(const struct data_vio *data_vio)
{
	int result = ASSERT(data_vio->hash_lock == NULL,
			    "must not already hold a hash lock");
	if (result != VDO_SUCCESS) {
		return result;
	}
	result = ASSERT(list_empty(&data_vio->hash_lock_entry),
			"must not already be a member of a hash lock ring");
	if (result != VDO_SUCCESS) {
		return result;
	}
	return ASSERT(data_vio->recovery_sequence_number == 0,
		      "must not hold a recovery lock when getting a hash lock");
}

/**
 * Acquire or share a lock on the hash (chunk name) of the data in a data_vio,
 * updating the data_vio to reference the lock. This must only be called in the
 * correct thread for the zone. In the unlikely case of a hash collision, this
 * function will succeed, but the data_vio will not get a lock reference.
 *
 * @param data_vio  The data_vio acquiring a lock on its chunk name
 **/
int vdo_acquire_hash_lock(struct data_vio *data_vio)
{
	struct hash_lock *lock;
	int result = assert_hash_lock_preconditions(data_vio);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_acquire_lock_from_hash_zone(data_vio->hash_zone,
						 &data_vio->chunk_name,
						 NULL,
						 &lock);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (is_hash_collision(lock, data_vio)) {
		/*
		 * Hash collisions are extremely unlikely, but the bogus
		 * dedupe would be a data corruption. Bypass dedupe entirely
		 * by leaving hash_lock unset.
		 * XXX clear hash_zone too?
		 */
		return VDO_SUCCESS;
	}

	set_hash_lock(data_vio, lock);
	return VDO_SUCCESS;
}

/**
 * Release a data_vio's share of a hash lock, if held, and null out the
 * data_vio's reference to it. This must only be called in the correct thread
 * for the hash zone.
 *
 * If the data_vio is the only one holding the lock, this also releases any
 * resources or locks used by the hash lock (such as a PBN read lock on a
 * block containing data with the same hash) and returns the lock to the hash
 * zone's lock pool.
 *
 * @param data_vio  The data_vio releasing its hash lock
 **/
void vdo_release_hash_lock(struct data_vio *data_vio)
{
	struct hash_lock *lock = data_vio->hash_lock;

	if (lock == NULL) {
		return;
	}

	set_hash_lock(data_vio, NULL);

	if (lock->reference_count > 0) {
		/* The lock is still in use by other data_vios. */
		return;
	}

	set_hash_lock_state(lock, VDO_HASH_LOCK_DESTROYING);
	vdo_return_lock_to_hash_zone(data_vio->hash_zone, lock);
}

/**
 * Transfer a data_vio's downgraded allocation PBN lock to the data_vio's hash
 * lock, converting it to a duplicate PBN lock.
 *
 * @param data_vio  The data_vio holding the allocation lock to transfer
 **/
static void transfer_allocation_lock(struct data_vio *data_vio)
{
	struct allocating_vio *allocating_vio =
		data_vio_as_allocating_vio(data_vio);
	struct pbn_lock *pbn_lock = allocating_vio->allocation_lock;
	struct hash_lock *hash_lock = data_vio->hash_lock;

	ASSERT_LOG_ONLY(data_vio->new_mapped.pbn ==
				get_data_vio_allocation(data_vio),
			"transferred lock must be for the block written");

	allocating_vio->allocation_lock = NULL;
	allocating_vio->allocation = VDO_ZERO_BLOCK;

	ASSERT_LOG_ONLY(vdo_is_pbn_read_lock(pbn_lock),
			"must have downgraded the allocation lock before transfer");

	hash_lock->duplicate = data_vio->new_mapped;
	data_vio->duplicate = data_vio->new_mapped;

	/*
	 * Since the lock is being transferred, the holder count doesn't change
	 * (and isn't even safe to examine on this thread).
	 */
	hash_lock->duplicate_lock = pbn_lock;
}

/**
 * Make a data_vio's hash lock a shared holder of the PBN lock on the
 * compressed block to which its data was just written. If the lock is still a
 * write lock (as it will be for the first share), it will be converted to a
 * read lock. This also reserves a reference count increment for the data_vio.
 *
 * @param data_vio  The data_vio which was just compressed
 * @param pbn_lock  The PBN lock on the compressed block
 **/
void vdo_share_compressed_write_lock(struct data_vio *data_vio,
				     struct pbn_lock *pbn_lock)
{
	bool claimed;

	ASSERT_LOG_ONLY(vdo_get_duplicate_lock(data_vio) == NULL,
			"a duplicate PBN lock should not exist when writing");
	ASSERT_LOG_ONLY(vdo_is_state_compressed(data_vio->new_mapped.state),
			"lock transfer must be for a compressed write");
	assert_data_vio_in_new_mapped_zone(data_vio);

	/* First sharer downgrades the lock. */
	if (!vdo_is_pbn_read_lock(pbn_lock)) {
		vdo_downgrade_pbn_write_lock(pbn_lock, true);
	}

	/*
	 * Get a share of the PBN lock, ensuring it cannot be released until
	 * after this data_vio has had a chance to journal a reference.
	 */
	data_vio->duplicate = data_vio->new_mapped;
	data_vio->hash_lock->duplicate = data_vio->new_mapped;
	set_duplicate_lock(data_vio->hash_lock, pbn_lock);

	/*
	 * Claim a reference for this data_vio. Necessary since another
	 * hash_lock might start deduplicating against it before our incRef.
	 */
	claimed = vdo_claim_pbn_lock_increment(pbn_lock);
	ASSERT_LOG_ONLY(claimed,
			"impossible to fail to claim an initial increment");
}
