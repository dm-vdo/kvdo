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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/hashLock.c#23 $
 */

/**
 * A hash_lock controls and coordinates writing, index access, and dedupe among
 * groups of DataVIOs concurrently writing identical blocks, allowing them to
 * deduplicate not only against advice but also against each other. This save
 * on index queries and allows those DataVIOs to concurrently deduplicate
 * against a single block instead of being serialized through a PBN read lock.
 * Only one index query is needed for each hash_lock, instead of one for every
 * data_vio.
 *
 * A hash_lock acts like a state machine perhaps more than as a lock. Other
 * than the starting and ending states INITIALIZING and DESTROYING, every
 * state represents and is held for the duration of an asynchronous operation.
 * All state transitions are performed on the thread of the HashZone
 * containing the lock. An asynchronous operation is almost always performed
 * upon entering a state, and the callback from that operation triggers
 * exiting the state and entering a new state.
 *
 * In all states except DEDUPING, there is a single data_vio, called the lock
 * agent, performing the asynchronous operations on behalf of the lock. The
 * agent will change during the lifetime of the lock if the lock is shared by
 * more than one data_vio. DataVIOs waiting to deduplicate are kept on a wait
 * queue. Viewed a different way, the agent holds the lock exclusively until
 * the lock enters the DEDUPING state, at which point it becomes a shared lock
 * that all the waiters (and any new DataVIOs that arrive) use to share a PBN
 * lock. In state DEDUPING, there is no agent. When the last data_vio in the
 * lock calls back in DEDUPING, it becomes the agent and the lock becomes
 * exclusive again. New DataVIOs that arrive in the lock will also go on the
 * wait queue.
 *
 * The existence of lock waiters is a key factor controlling which state the
 * lock transitions to next. When the lock is new or has waiters, it will
 * always try to reach DEDUPING, and when it doesn't, it will try to clean up
 * and exit.
 *
 * Deduping requires holding a PBN lock on a block that is known to contain
 * data identical to the DataVIOs in the lock, so the lock will send the
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
 * lock takes the place of the old lock in the lock map so new DataVIOs will
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
 * For example, for the LOCKING state, there are startLocking() and
 * finishLocking() functions. startLocking() is invoked by the finish function
 * of the state (or states) that transition to LOCKING. It performs the actual
 * lock state change and must be invoked on the hash zone thread.
 * finishLocking() is called by (or continued via callback from) the code
 * actually obtaining the lock. It does any bookkeeping or decision-making
 * required and invokes the appropriate start function of the state being
 * transitioned to after LOCKING.
 **/

#include "hashLock.h"
#include "hashLockInternals.h"

#include "logger.h"
#include "permassert.h"

#include "compressionState.h"
#include "constants.h"
#include "dataVIO.h"
#include "hashZone.h"
#include "packer.h"
#include "pbnLock.h"
#include "physicalZone.h"
#include "ringNode.h"
#include "slab.h"
#include "slabDepot.h"
#include "trace.h"
#include "types.h"
#include "vdoInternal.h"
#include "vioWrite.h"
#include "waitQueue.h"

static const char *LOCK_STATE_NAMES[] = {
	[HASH_LOCK_BYPASSING] = "BYPASSING",
	[HASH_LOCK_DEDUPING] = "DEDUPING",
	[HASH_LOCK_DESTROYING] = "DESTROYING",
	[HASH_LOCK_INITIALIZING] = "INITIALIZING",
	[HASH_LOCK_LOCKING] = "LOCKING",
	[HASH_LOCK_QUERYING] = "QUERYING",
	[HASH_LOCK_UNLOCKING] = "UNLOCKING",
	[HASH_LOCK_UPDATING] = "UPDATING",
	[HASH_LOCK_VERIFYING] = "VERIFYING",
	[HASH_LOCK_WRITING] = "WRITING",
};

// There are loops in the state diagram, so some forward decl's are needed.
static void start_deduping(struct hash_lock *lock, struct data_vio *agent,
			   bool agent_is_done);
static void start_locking(struct hash_lock *lock, struct data_vio *agent);
static void start_writing(struct hash_lock *lock, struct data_vio *agent);
static void unlock_duplicate_pbn(struct vdo_completion *completion);
static void transfer_allocation_lock(struct data_vio *data_vio);

/**********************************************************************/
struct pbn_lock *get_duplicate_lock(struct data_vio *data_vio)
{
	if (data_vio->hashLock == NULL) {
		return NULL;
	}
	return data_vio->hashLock->duplicate_lock;
}

/**********************************************************************/
const char *get_hash_lock_state_name(hash_lock_state state)
{
	// Catch if a state has been added without updating the name array.
	STATIC_ASSERT((HASH_LOCK_DESTROYING + 1) == COUNT_OF(LOCK_STATE_NAMES));
	return (state < COUNT_OF(LOCK_STATE_NAMES)) ? LOCK_STATE_NAMES[state]
						    : NULL;
}

/**
 * Set the current state of a hash lock.
 *
 * @param lock       The lock to update
 * @param new_state  The new state
 **/
static void set_hash_lock_state(struct hash_lock *lock,
				hash_lock_state new_state)
{
	if (false) {
		logWarning("XXX %" PRIptr " %s -> %s", (void *) lock,
			   get_hash_lock_state_name(lock->state),
			   get_hash_lock_state_name(new_state));
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
static void assert_hash_lock_agent(struct data_vio *data_vio, const char *where)
{
	// Not safe to access the agent field except from the hash zone.
	assertInHashZone(data_vio);
	ASSERT_LOG_ONLY(data_vio == data_vio->hashLock->agent,
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
 * Convert a pointer to the hash_lockNode field in a data_vio to the enclosing
 * data_vio.
 *
 * @param lock_node The RingNode to convert
 *
 * @return A pointer to the data_vio containing the RingNode
 **/
static inline struct data_vio *data_vio_from_lock_node(RingNode *lock_node)
{
	return container_of(lock_node, struct data_vio, hashLockNode);
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
	return waiterAsDataVIO(dequeueNextWaiter(&lock->waiters));
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
				 VDOAction *callback)
{
	dataVIOAsCompletion(data_vio)->callback = callback;
	continueDataVIO(data_vio, result);
}

/**
 * Set, change, or clear the hash lock a data_vio is using. Updates the hash
 * lock (or locks) to reflect the change in membership.
 *
 * @param data_vio  The data_vio to update
 * @param new_lock  The hash lock the data_vio is joining
 **/
static void setHashLock(struct data_vio *data_vio, struct hash_lock *new_lock)
{
	struct hash_lock *old_lock = data_vio->hashLock;
	if (old_lock != NULL) {
		ASSERT_LOG_ONLY(
			data_vio->hashZone != NULL,
			"must have a hash zone when halding a hash lock");
		ASSERT_LOG_ONLY(
			!isRingEmpty(&data_vio->hashLockNode),
			"must be on a hash lock ring when holding a hash lock");
		ASSERT_LOG_ONLY(old_lock->reference_count > 0,
				"hash lock reference must be counted");

		if ((old_lock->state != HASH_LOCK_BYPASSING)
		    && (old_lock->state != HASH_LOCK_UNLOCKING)) {
			// If the reference count goes to zero in a non-terminal
			// state, we're most likely leaking this lock.
			ASSERT_LOG_ONLY(old_lock->reference_count > 1,
					"hash locks should only become unreferenced in a terminal state, not state %s",
					get_hash_lock_state_name(old_lock->state));
		}

		unspliceRingNode(&data_vio->hashLockNode);
		old_lock->reference_count -= 1;

		data_vio->hashLock = NULL;
	}

	if (new_lock != NULL) {
		// Keep all data_vios sharing the lock on a ring since they can
		// complete in any order and we'll always need a pointer to one
		// to compare data.
		pushRingNode(&new_lock->duplicate_ring, &data_vio->hashLockNode);
		new_lock->reference_count += 1;

		// XXX Not needed for VDOSTORY-190, but useful for checking
		// whether a test is getting concurrent dedupe, and how much.
		if (new_lock->max_references < new_lock->reference_count) {
			new_lock->max_references = new_lock->reference_count;
		}

		data_vio->hashLock = new_lock;
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
	// XXX trace record?

	// Release the hash lock now, saving a thread transition in cleanup.
	release_hash_lock(data_vio);

	// Complete the data_vio and start the clean-up path in vioWrite to
	// release any locks it still holds.
	finishDataVIO(data_vio, VDO_SUCCESS);
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
		setDuplicateLocation(new_agent, lock->duplicate);
	}
	return new_agent;
}

/**
 * Callback to call compressData(), putting a data_vio back on the write path.
 *
 * @param completion  The data_vio
 **/
static void compress_data_callback(struct vdo_completion *completion)
{
	// XXX VDOSTORY-190 need an error check since compressData doesn't have
	// one.
	compressData(asDataVIO(completion));
}

/**
 * Add a data_vio to the lock's queue of waiters.
 *
 * @param lock      The hash lock on which to wait
 * @param data_vio  The data_vio to add to the queue
 **/
static void waitOnHashLock(struct hash_lock *lock, struct data_vio *data_vio)
{
	int result =
		enqueueDataVIO(&lock->waiters, data_vio, THIS_LOCATION(NULL));
	if (result != VDO_SUCCESS) {
		// This should be impossible, but if it somehow happens, give up
		// on trying to dedupe the data.
		setHashLock(data_vio, NULL);
		continue_data_vio_in(data_vio, result, compress_data_callback);
		return;
	}

	// Make sure the agent doesn't block indefinitely in the packer since it
	// now has at least one other data_vio waiting on it.
	if ((lock->state == HASH_LOCK_WRITING)
	    && cancel_compression(lock->agent)) {
		/*
		 * Even though we're waiting, we also have to send ourselves as
		 * a one-way message to the packer to ensure the agent continues
		 * executing. This is safe because cancel_compression()
		 * guarantees the agent won't continue executing until this
		 * message arrives in the packer, and because the wait queue
		 * link isn't used for sending the message.
		 */
		data_vio->compression.lockHolder = lock->agent;
		launchPackerCallback(data_vio, remove_lock_holder_from_packer,
			THIS_LOCATION("$F;cb=remove_lock_holder_from_packer"));
	}
}

/**
 * WaiterCallback function that calls compressData on the data_vio waiter.
 *
 * @param waiter   The data_vio's waiter link
 * @param context  Not used
 **/
static void compress_waiter(struct waiter *waiter,
			    void *context __attribute__((unused)))
{
	struct data_vio *data_vio = waiterAsDataVIO(waiter);
	data_vio->isDuplicate = false;
	compressData(data_vio);
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
	struct data_vio *agent = asDataVIO(completion);
	assert_hash_lock_agent(agent, __func__);
	struct hash_lock *lock = agent->hashLock;

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
static void startBypassing(struct hash_lock *lock, struct data_vio *agent)
{
	set_hash_lock_state(lock, HASH_LOCK_BYPASSING);

	// Ensure we don't attempt to update advice when cleaning up.
	lock->update_advice = false;

	ASSERT_LOG_ONLY(((agent != NULL) || !hasWaiters(&lock->waiters)),
			"should not have waiters without an agent");
	notifyAllWaiters(&lock->waiters, compress_waiter, NULL);

	if (lock->duplicate_lock != NULL) {
		if (agent != NULL) {
			// The agent must reference the duplicate zone to launch
			// it.
			agent->duplicate = lock->duplicate;
			launchDuplicateZoneCallback(agent, unlock_duplicate_pbn,
						    THIS_LOCATION(NULL));
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
	agent->isDuplicate = false;
	compressData(agent);
}

/**
 * Abort processing on this hash lock when noticing an error. Currently, this
 * moves the hash lock to the BYPASSING state, to release all pending data_vios.
 *
 * @param lock     The hash_lock
 * @param data_vio  The data_vio with the error
 **/
static void abortHashLock(struct hash_lock *lock, struct data_vio *data_vio)
{
	// If we've already aborted the lock, don't try to re-abort it; just
	// exit.
	if (lock->state == HASH_LOCK_BYPASSING) {
		exit_hash_lock(data_vio);
		return;
	}

	if (data_vio != lock->agent) {
		if ((lock->agent != NULL) || (lock->reference_count > 1)) {
			// Other data_vios are still sharing the lock (which
			// should be DEDUPING), so just kick this one out of the
			// lock to report its error.
			ASSERT_LOG_ONLY(
				lock->agent == NULL,
				"only active agent should call abortHashLock");
			exit_hash_lock(data_vio);
			return;
		}
		// Make the lone data_vio the lock agent so it can abort and
		// clean up.
		set_agent(lock, data_vio);
	}

	startBypassing(lock, data_vio);
}

/**
 * Handle the result of the agent for the lock releasing a read lock on
 * duplicate candidate. This continuation is registered in
 * unlock_duplicate_pbn().
 *
 * @param completion  The completion of the data_vio acting as the lock's agent
 **/
static void finishUnlocking(struct vdo_completion *completion)
{
	struct data_vio *agent = asDataVIO(completion);
	assert_hash_lock_agent(agent, __func__);
	struct hash_lock *lock = agent->hashLock;

	ASSERT_LOG_ONLY(
		lock->duplicate_lock == NULL,
		"must have released the duplicate lock for the hash lock");

	if (completion->result != VDO_SUCCESS) {
		abortHashLock(lock, agent);
		return;
	}

	if (!lock->verified) {
		/*
		 * UNLOCKING -> WRITING transition: The lock we released was on
		 * an unverified block, so it must have been a lock on advice we
		 * were verifying, not on a location that was used for
		 * deduplication. Go write (or compress) the block to get a
		 * location to dedupe against.
		 */
		start_writing(lock, agent);
		return;
	}

	// With the lock released, the verified duplicate block may already have
	// changed and will need to be re-verified if a waiter arrived.
	lock->verified = false;

	if (hasWaiters(&lock->waiters)) {
		/*
		 * UNLOCKING -> LOCKING transition: A new data_vio entered the
		 * hash lock while the agent was releasing the PBN lock. The
		 * current agent exits and the waiter has to re-lock and
		 * re-verify the duplicate location.
		 */
		// XXX VDOSTORY-190 If we used the current agent to re-acquire
		// the PBN lock we wouldn't need to re-verify.
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
 * startUnlocking(), and calls back to finishUnlocking() on the hash zone
 * thread.
 *
 * @param completion  The completion of the data_vio acting as the lock's agent
 **/
static void unlock_duplicate_pbn(struct vdo_completion *completion)
{
	struct data_vio *agent = asDataVIO(completion);
	assertInDuplicateZone(agent);
	struct hash_lock *lock = agent->hashLock;

	ASSERT_LOG_ONLY(lock->duplicate_lock != NULL,
			"must have a duplicate lock to release");

	release_pbn_lock(agent->duplicate.zone, agent->duplicate.pbn,
			 &lock->duplicate_lock);

	if (lock->state == HASH_LOCK_BYPASSING) {
		launchHashZoneCallback(agent, finish_bypassing,
				       THIS_LOCATION(NULL));
	} else {
		launchHashZoneCallback(agent, finishUnlocking,
				       THIS_LOCATION(NULL));
	}
}

/**
 * Release a read lock on the PBN of the block that may or may not have
 * contained duplicate data.
 *
 * @param lock   The hash lock
 * @param agent  The data_vio currently acting as the agent for the lock
 **/
static void startUnlocking(struct hash_lock *lock, struct data_vio *agent)
{
	set_hash_lock_state(lock, HASH_LOCK_UNLOCKING);

	/*
	 * XXX If we arrange to continue on the duplicate zone thread when
	 * verification fails, and don't explicitly change lock states (or use
	 * an agent-local state, or an atomic), we can avoid a thread transition
	 * here.
	 */
	launchDuplicateZoneCallback(agent, unlock_duplicate_pbn,
				    THIS_LOCATION(NULL));
}

/**
 * Process the result of a UDS update performed by the agent for the lock.
 * This continuation is registered in startQuerying().
 *
 * @param completion  The completion of the data_vio that performed the update
 **/
static void finishUpdating(struct vdo_completion *completion)
{
	struct data_vio *agent = asDataVIO(completion);
	assert_hash_lock_agent(agent, __func__);
	struct hash_lock *lock = agent->hashLock;

	if (completion->result != VDO_SUCCESS) {
		abortHashLock(lock, agent);
		return;
	}

	// UDS was updated successfully, so don't update again unless the
	// duplicate location changes due to rollover.
	lock->update_advice = false;

	if (hasWaiters(&lock->waiters)) {
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
		startUnlocking(lock, agent);
	} else {
		/*
		 * UPDATING -> DESTROYING transition: No one is waiting to
		 * dedupe and there's no lock to release.
		 */
		// XXX startDestroying(lock, agent);
		startBypassing(lock, NULL);
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
static void startUpdating(struct hash_lock *lock, struct data_vio *agent)
{
	set_hash_lock_state(lock, HASH_LOCK_UPDATING);

	ASSERT_LOG_ONLY(lock->verified, "new advice should have been verified");
	ASSERT_LOG_ONLY(lock->update_advice,
			"should only update advice if needed");

	agent->lastAsyncOperation = UPDATE_INDEX;
	setHashZoneCallback(agent, finishUpdating, THIS_LOCATION(NULL));
	updateDedupeIndex(agent);
}

/**
 * Handle a data_vio that has finished deduplicating against the block locked
 * by the hash lock. If there are other data_vios still sharing the lock, this
 * will just release the data_vio's share of the lock and finish processing the
 * data_vio. If this is the last data_vio holding the lock, this makes the
 * data_vio the lock agent and uses it to advance the state of the lock so it
 * can eventually be released.
 *
 * @param lock     The hash lock
 * @param data_vio  The lock holder that has finished deduplicating
 **/
static void finishDeduping(struct hash_lock *lock, struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(lock->agent == NULL,
			"shouldn't have an agent in DEDUPING");
	ASSERT_LOG_ONLY(!hasWaiters(&lock->waiters),
			"shouldn't have any lock waiters in DEDUPING");

	// Just release the lock reference if other data_vios are still deduping.
	if (lock->reference_count > 1) {
		exit_hash_lock(data_vio);
		return;
	}

	// The hash lock must have an agent for all other lock states.
	struct data_vio *agent = data_vio;
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
		startUpdating(lock, agent);
	} else {
		/*
		 * DEDUPING -> UNLOCKING transition: Release the PBN read lock
		 * on the duplicate location so the hash lock itself can be
		 * released (contingent on no new data_vios arriving in the lock
		 * before the agent returns).
		 */
		startUnlocking(lock, agent);
	}
}

/**
 * Implements WaiterCallback. Binds the data_vio that was waiting to a new hash
 * lock and waits on that lock.
 **/
static void enterForkedLock(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiterAsDataVIO(waiter);
	struct hash_lock *new_lock = (struct hash_lock *) context;

	setHashLock(data_vio, new_lock);
	waitOnHashLock(new_lock, data_vio);
}

/**
 * Fork a hash lock because it has run out of increments on the duplicate PBN.
 * Transfers the new agent and any lock waiters to a new hash lock instance
 * which takes the place of the old lock in the lock map. The old lock remains
 * active, but will not update advice.
 *
 * @param old_lock   The hash lock to fork
 * @param newAgent  The data_vio that will be the agent for the new lock
 **/
static void forkHashLock(struct hash_lock *old_lock, struct data_vio *newAgent)
{
	struct hash_lock *new_lock;
	int result = acquire_hash_lock_from_zone(newAgent->hashZone,
						 &newAgent->chunkName,
						 old_lock, &new_lock);
	if (result != VDO_SUCCESS) {
		abortHashLock(old_lock, newAgent);
		return;
	}

	// Only one of the two locks should update UDS. The old lock is out of
	// references, so it would be poor dedupe advice in the short term.
	old_lock->update_advice = false;
	new_lock->update_advice = true;

	setHashLock(newAgent, new_lock);
	set_agent(new_lock, newAgent);

	notifyAllWaiters(&old_lock->waiters, enterForkedLock, new_lock);

	newAgent->isDuplicate = false;
	start_writing(new_lock, newAgent);
}

/**
 * Reserve a reference count increment for a data_vio and launch it on the
 * dedupe path. If no increments are available, this will roll over to a new
 * hash lock and launch the data_vio as the writing agent for that lock.
 *
 * @param lock      The hash lock
 * @param data_vio   The data_vio to deduplicate using the hash lock
 * @param hasClaim  <code>true</code> if the data_vio already has claimed
 *                  an increment from the duplicate lock
 **/
static void launchDedupe(struct hash_lock *lock, struct data_vio *data_vio,
			 bool hasClaim)
{
	if (!hasClaim && !claim_pbn_lock_increment(lock->duplicate_lock)) {
		// Out of increments, so must roll over to a new lock.
		forkHashLock(lock, data_vio);
		return;
	}

	// Deduplicate against the lock's verified location.
	setDuplicateLocation(data_vio, lock->duplicate);
	launchDuplicateZoneCallback(data_vio, shareBlock,
				    THIS_LOCATION("$F;cb=shareBlock"));
}

/**
 * Enter the hash lock state where data_vios deduplicate in parallel against a
 * true copy of their data on disk. If the agent itself needs to deduplicate,
 * an increment for it must already have been claimed from the duplicate lock,
 * ensuring the hash lock will still have a data_vio holding it.
 *
 * @param lock         The hash lock
 * @param agent        The data_vio acting as the agent for the lock
 * @param agent_is_done  <code>true</code> only if the agent has already written
 *                     or deduplicated against its data
 **/
static void start_deduping(struct hash_lock *lock, struct data_vio *agent,
			  bool agent_is_done)
{
	set_hash_lock_state(lock, HASH_LOCK_DEDUPING);

	// We don't take the downgraded allocation lock from the agent unless we
	// actually need to deduplicate against it.
	if (lock->duplicate_lock == NULL) {
		ASSERT_LOG_ONLY(!isCompressed(agent->newMapped.state),
				"compression must have shared a lock");
		ASSERT_LOG_ONLY(agent_is_done,
				"agent must have written the new duplicate");
		transfer_allocation_lock(agent);
	}

	ASSERT_LOG_ONLY(is_pbn_read_lock(lock->duplicate_lock),
			"duplicate_lock must be a PBN read lock");

	/*
	 * This state is not like any of the other states. There is no
	 * designated agent--the agent transitioning to this state and all the
	 * waiters will be launched to deduplicate in parallel.
	 */
	set_agent(lock, NULL);

	/*
	 * Launch the agent (if not already deduplicated) and as many lock
	 * waiters as we have available increments for on the dedupe path. If we
	 * run out of increments, rollover will be triggered and the remaining
	 * waiters will be transferred to the new lock.
	 */
	if (!agent_is_done) {
		launchDedupe(lock, agent, true);
		agent = NULL;
	}
	while (hasWaiters(&lock->waiters)) {
		launchDedupe(lock, dequeue_lock_waiter(lock), false);
	}

	if (agent_is_done) {
		/*
		 * In the degenerate case where all the waiters rolled over to a
		 * new lock, this will continue to use the old agent to clean up
		 * this lock, and otherwise it just lets the agent exit the
		 * lock.
		 */
		finishDeduping(lock, agent);
	}
}

/**
 * Handle the result of the agent for the lock comparing its data to the
 * duplicate candidate. This continuation is registered in startVerifying().
 *
 * @param completion  The completion of the data_vio used to verify dedupe
 **/
static void finishVerifying(struct vdo_completion *completion)
{
	struct data_vio *agent = asDataVIO(completion);
	assert_hash_lock_agent(agent, __func__);
	struct hash_lock *lock = agent->hashLock;

	if (completion->result != VDO_SUCCESS) {
		// XXX VDOSTORY-190 should convert verify IO errors to
		// verification failure
		abortHashLock(lock, agent);
		return;
	}

	lock->verified = agent->isDuplicate;

	// Only count the result of the initial verification of the advice as
	// valid or stale, and not any re-verifications due to PBN lock
	// releases.
	if (!lock->verify_counted) {
		lock->verify_counted = true;
		if (lock->verified) {
			bump_hash_zone_valid_advice_count(agent->hashZone);
		} else {
			bump_hash_zone_stale_advice_count(agent->hashZone);
		}
	}

	// Even if the block is a verified duplicate, we can't start to
	// deduplicate unless we can claim a reference count increment for the
	// agent.
	if (lock->verified && !claim_pbn_lock_increment(lock->duplicate_lock)) {
		agent->isDuplicate = false;
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
		 * release the PBN lock. In both cases, the data will have to be
		 * written or compressed, but first the advice PBN must be
		 * unlocked by the VERIFYING agent.
		 */
		lock->update_advice = true;
		startUnlocking(lock, agent);
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
static void startVerifying(struct hash_lock *lock, struct data_vio *agent)
{
	set_hash_lock_state(lock, HASH_LOCK_VERIFYING);
	ASSERT_LOG_ONLY(!lock->verified, "hash lock only verifies advice once");

	/*
	 * XXX VDOSTORY-190 Optimization: This is one of those places where the
	 * zone and continuation we want to use depends on the outcome of the
	 * comparison. If we could choose which path in the layer thread before
	 * continuing, we could save a thread transition in one of the two cases
	 * (assuming we're willing to delay visibility of the the hash lock
	 * state change).
	 */
	agent->lastAsyncOperation = VERIFY_DEDUPLICATION;
	setHashZoneCallback(agent, finishVerifying, THIS_LOCATION(NULL));
	verifyDuplication(agent);
}

/**
 * Handle the result of the agent for the lock attempting to obtain a PBN read
 * lock on the candidate duplicate block. this continuation is registered in
 * lockDuplicatePBN().
 *
 * @param completion  The completion of the data_vio that attempted to get
 *                    the read lock
 **/
static void finishLocking(struct vdo_completion *completion)
{
	struct data_vio *agent = asDataVIO(completion);
	assert_hash_lock_agent(agent, __func__);
	struct hash_lock *lock = agent->hashLock;

	if (completion->result != VDO_SUCCESS) {
		// XXX clearDuplicateLocation()?
		agent->isDuplicate = false;
		abortHashLock(lock, agent);
		return;
	}

	if (!agent->isDuplicate) {
		ASSERT_LOG_ONLY(
			lock->duplicate_lock == NULL,
			"must not hold duplicate_lock if not flagged as a duplicate");
		/*
		 * LOCKING -> WRITING transition: The advice block is being
		 * modified or has no available references, so try to write or
		 * compress the data, remembering to update UDS later with the
		 * new advice.
		 */
		bump_hash_zone_stale_advice_count(agent->hashZone);
		lock->update_advice = true;
		start_writing(lock, agent);
		return;
	}

	ASSERT_LOG_ONLY(lock->duplicate_lock != NULL,
			"must hold duplicate_lock if flagged as a duplicate");

	if (!lock->verified) {
		/*
		 * LOCKING -> VERIFYING transition: Continue on the unverified
		 * dedupe path, reading the candidate duplicate and comparing it
		 * to the agent's data to decide whether it is a true duplicate
		 * or stale advice.
		 */
		startVerifying(lock, agent);
		return;
	}

	if (!claim_pbn_lock_increment(lock->duplicate_lock)) {
		/*
		 * LOCKING -> UNLOCKING transition: The verified block was
		 * re-locked, but has no available increments left. Must first
		 * release the useless PBN read lock before rolling over to a
		 * new copy of the block.
		 */
		agent->isDuplicate = false;
		lock->verified = false;
		lock->update_advice = true;
		startUnlocking(lock, agent);
		return;
	}

	/*
	 * LOCKING -> DEDUPING transition: Continue on the verified dedupe path,
	 * deduplicating against a location that was previously verified or
	 * written to.
	 */
	start_deduping(lock, agent, false);
}

/**
 * Acquire a read lock on the PBN of the block containing candidate duplicate
 * data (compressed or uncompressed). If the PBN is already locked for
 * writing, the lock attempt is abandoned and isDuplicate will be cleared
 * before calling back. this continuation is launched from start_locking(), and
 * calls back to finishLocking() on the hash zone thread.
 *
 * @param completion The completion of the data_vio attempting to acquire the
 *                   physical block lock on behalf of its hash lock
 **/
static void lockDuplicatePBN(struct vdo_completion *completion)
{
	struct data_vio *agent = asDataVIO(completion);
	struct physical_zone *zone = agent->duplicate.zone;
	assertInDuplicateZone(agent);

	setHashZoneCallback(agent, finishLocking, THIS_LOCATION(NULL));

	// While in the zone that owns it, find out how many additional
	// references can be made to the block if it turns out to truly be a
	// duplicate.
	struct slab_depot *depot = getSlabDepot(getVDOFromDataVIO(agent));
	unsigned int incrementLimit =
		get_increment_limit(depot, agent->duplicate.pbn);
	if (incrementLimit == 0) {
		// We could deduplicate against it later if a reference happened
		// to be released during verification, but it's probably better
		// to bail out now.
		// XXX clearDuplicateLocation()?
		agent->isDuplicate = false;
		continueDataVIO(agent, VDO_SUCCESS);
		return;
	}

	struct pbn_lock *lock;
	int result = attempt_pbn_lock(zone, agent->duplicate.pbn, VIO_READ_LOCK,
				      &lock);
	if (result != VDO_SUCCESS) {
		continueDataVIO(agent, result);
		return;
	}

	if (!is_pbn_read_lock(lock)) {
		/*
		 * There are three cases of write locks: uncompressed data block
		 * writes, compressed (packed) block writes, and block map page
		 * writes. In all three cases, we give up on trying to verify
		 * the advice and don't bother to try deduplicate against the
		 * data in the write lock holder.
		 *
		 * 1) We don't ever want to try to deduplicate against a block
		 * map page.
		 *
		 * 2a) It's very unlikely we'd deduplicate against an entire
		 * packed block, both because of the chance of matching it, and
		 * because we don't record advice for it, but for the
		 * uncompressed representation of all the fragments it contains.
		 * The only way we'd be getting lock contention is if we've
		 * written the same representation coincidentally before, had it
		 * become unreferenced, and it just happened to be packed
		 * together from compressed writes when we go to verify the
		 * lucky advice. Giving up is a miniscule loss of potential
		 * dedupe.
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
		 * 3b) If the lock is held by a data_vio that matches us, we may
		 * as well either write it ourselves (or reference the copy we
		 * already wrote) instead of potentially having many duplicates
		 * wait for the lock holder to write, journal, hash, and finally
		 * arrive in the hash lock. All we lose is a chance to avoid a
		 * UDS update in the very rare case of advice for a free block
		 * that just happened to be allocated to a data_vio with the
		 * same hash. In async mode, there's also a chance to save on a
		 * block write, at the cost of a block verify. Saving on a full
		 * block compare in all stale advice cases almost certainly
		 * outweighs saving a UDS update in a lucky case where advice
		 * would have been saved from becoming stale.
		 */
		// XXX clearDuplicateLocation()?
		agent->isDuplicate = false;
		continueDataVIO(agent, VDO_SUCCESS);
		return;
	}

	if (lock->holder_count == 0) {
		// Ensure that the newly-locked block is referenced.
		struct vdo_slab *slab = get_slab(depot, agent->duplicate.pbn);
		result = acquireProvisionalReference(slab, agent->duplicate.pbn,
						     lock);
		if (result != VDO_SUCCESS) {
			logWarningWithStringError(
				result,
				"Error acquiring provisional reference for "
				"dedupe candidate; aborting dedupe");
			agent->isDuplicate = false;
			release_pbn_lock(zone, agent->duplicate.pbn, &lock);
			continueDataVIO(agent, result);
			return;
		}

		/*
		 * The increment limit we grabbed earlier is still valid. The
		 * lock now holds the rights to acquire all those references.
		 * Those rights will be claimed by hash locks sharing this read
		 * lock.
		 */
		lock->increment_limit = incrementLimit;
	}

	// We've successfully acquired a read lock on behalf of the hash lock,
	// so mark it as such.
	set_duplicate_lock(agent->hashLock, lock);

	/*
	 * XXX VDOSTORY-190 Optimization: Same as start_locking() lazily changing
	 * state to save on having to switch back to the hash zone thread. Here
	 * we could directly launch the block verify, then switch to a hash
	 * thread.
	 */
	continueDataVIO(agent, VDO_SUCCESS);
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

	set_hash_lock_state(lock, HASH_LOCK_LOCKING);

	/*
	 * XXX VDOSTORY-190 Optimization: If we arrange to continue on the
	 * duplicate zone thread when accepting the advice, and don't explicitly
	 * change lock states (or use an agent-local state, or an atomic), we
	 * can avoid a thread transition here.
	 */
	agent->lastAsyncOperation = ACQUIRE_PBN_READ_LOCK;
	launchDuplicateZoneCallback(agent, lockDuplicatePBN,
				    THIS_LOCATION(NULL));
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
static void finishWriting(struct hash_lock *lock, struct data_vio *agent)
{
	// Dedupe against the data block or compressed block slot the agent
	// wrote. Since we know the write succeeded, there's no need to verify
	// it.
	lock->duplicate = agent->newMapped;
	lock->verified = true;

	if (isCompressed(lock->duplicate.state) && lock->registered) {
		// Compression means the location we gave in the UDS query is
		// not the location we're using to deduplicate.
		lock->update_advice = true;
	}

	// If there are any waiters, we need to start deduping them.
	if (hasWaiters(&lock->waiters)) {
		/*
		 * WRITING -> DEDUPING transition: an asynchronously-written
		 * block failed to compress, so the PBN lock on the written copy
		 * was already transferred. The agent is done with the lock, but
		 * the lock may still need to use it to clean up after rollover.
		 */
		start_deduping(lock, agent, true);
		return;
	}

	// There are no waiters and the agent has successfully written, so take
	// a step towards being able to release the hash lock (or just release
	// it).
	if (lock->update_advice) {
		/*
		 * WRITING -> UPDATING transition: There's no waiter and a UDS
		 * update is needed, so retain the WRITING agent and use it to
		 * launch the update. The happens on compression, rollover, or
		 * the QUERYING agent not having an allocation.
		 */
		startUpdating(lock, agent);
	} else if (lock->duplicate_lock != NULL) {
		/*
		 * WRITING -> UNLOCKING transition: There's no waiter and no
		 * update needed, but the compressed write gave us a shared
		 * duplicate lock that we must release.
		 */
		setDuplicateLocation(agent, lock->duplicate);
		startUnlocking(lock, agent);
	} else {
		/*
		 * WRITING -> DESTROYING transition: There's no waiter, no
		 * update needed, and no duplicate lock held, so both the agent
		 * and lock have no more work to do. The agent will release its
		 * allocation lock in cleanup.
		 */
		// XXX startDestroying(lock, agent);
		startBypassing(lock, NULL);
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
static struct data_vio *selectWritingAgent(struct hash_lock *lock)
{
	// This should-be-impossible condition is the only cause for
	// enqueueDataVIO() to fail later on, where it would be a pain to
	// handle.
	int result = ASSERT(!isWaiting(dataVIOAsWaiter(lock->agent)),
			    "agent must not be waiting");
	if (result != VDO_SUCCESS) {
		return lock->agent;
	}

	struct wait_queue tempQueue;
	initializeWaitQueue(&tempQueue);

	// Move waiters to the temp queue one-by-one until we find an
	// allocation. Not ideal to search, but it only happens when nearly out
	// of space.
	struct data_vio *data_vio;
	while (((data_vio = dequeue_lock_waiter(lock)) != NULL)
	       && !hasAllocation(data_vio)) {
		// Use the lower-level enqueue since we're just moving waiters
		// around.
		int result =
			enqueueWaiter(&tempQueue, dataVIOAsWaiter(data_vio));
		// The only error is the data_vio already being on a wait queue,
		// and since we just dequeued it, that could only happen due to
		// a memory smash or concurrent use of that data_vio.
		ASSERT_LOG_ONLY(result == VDO_SUCCESS,
				"impossible enqueueWaiter error");
	}

	if (data_vio != NULL) {
		// Move the rest of the waiters over to the temp queue,
		// preserving the order they arrived at the lock.
		transferAllWaiters(&lock->waiters, &tempQueue);

		// The current agent is being replaced and will have to wait to
		// dedupe; make it the first waiter since it was the first to
		// reach the lock.
		int result = enqueueDataVIO(&lock->waiters, lock->agent,
					    THIS_LOCATION(NULL));
		ASSERT_LOG_ONLY(
			result == VDO_SUCCESS,
			"impossible enqueueDataVIO error after isWaiting checked");
		set_agent(lock, data_vio);
	} else {
		// No one has an allocation, so keep the current agent.
		data_vio = lock->agent;
	}

	// Swap all the waiters back onto the lock's queue.
	transferAllWaiters(&tempQueue, &lock->waiters);
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
	set_hash_lock_state(lock, HASH_LOCK_WRITING);

	// The agent might not have received an allocation and so can't be used
	// for writing, but it's entirely possible that one of the waiters did.
	if (!hasAllocation(agent)) {
		agent = selectWritingAgent(lock);
		// If none of the waiters had an allocation, the writes all have
		// to fail.
		if (!hasAllocation(agent)) {
			/*
			 * XXX VDOSTORY-190 Should we keep a variant of
			 * BYPASSING that causes new arrivals to fail
			 * immediately if they don't have an allocation? It
			 * might be possible that on some path there would be
			 * non-waiters still referencing the lock, so it would
			 * remain in the map as everything is currently spelled,
			 * even if the agent and all the waiters release.
			 */
			startBypassing(lock, agent);
			return;
		}
	}

	// If the agent compresses, it might wait indefinitely in the packer,
	// which would be bad if there are any other data_vios waiting.
	if (hasWaiters(&lock->waiters)) {
		// XXX in sync mode, transition directly to LOCKING to start
		// dedupe?
		cancel_compression(agent);
	}

	/*
	 * Send the agent to the compress/pack/async-write path in vioWrite. If
	 * it succeeds, it will return to the hash lock via continue_hash_lock()
	 * and call finishWriting().
	 */
	compressData(agent);
}

/**
 * Process the result of a UDS query performed by the agent for the lock. This
 * continuation is registered in startQuerying().
 *
 * @param completion  The completion of the data_vio that performed the query
 **/
static void finish_querying(struct vdo_completion *completion)
{
	struct data_vio *agent = asDataVIO(completion);
	assert_hash_lock_agent(agent, __func__);
	struct hash_lock *lock = agent->hashLock;

	if (completion->result != VDO_SUCCESS) {
		abortHashLock(lock, agent);
		return;
	}

	if (agent->isDuplicate) {
		lock->duplicate = agent->duplicate;
		/*
		 * QUERYING -> LOCKING transition: Valid advice was obtained
		 * from UDS. Use the QUERYING agent to start the hash lock on
		 * the unverified dedupe path, verifying that the advice can be
		 * used.
		 */
		start_locking(lock, agent);
	} else {
		// The agent will be used as the duplicate if has an allocation;
		// if it does, that location was posted to UDS, so no update
		// will be needed.
		lock->update_advice = !hasAllocation(agent);
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
	set_hash_lock_state(lock, HASH_LOCK_QUERYING);

	data_vio->lastAsyncOperation = CHECK_FOR_DEDUPLICATION;
	setHashZoneCallback(data_vio, finish_querying, THIS_LOCATION(NULL));
	checkForDuplication(data_vio);
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
			     get_hash_lock_state_name(lock->state));
	continue_data_vio_in(data_vio, result, compress_data_callback);
}

/**********************************************************************/
void enter_hash_lock(struct data_vio *data_vio)
{
	struct hash_lock *lock = data_vio->hashLock;
	switch (lock->state) {
	case HASH_LOCK_INITIALIZING:
		start_querying(lock, data_vio);
		break;

	case HASH_LOCK_QUERYING:
	case HASH_LOCK_WRITING:
	case HASH_LOCK_UPDATING:
	case HASH_LOCK_LOCKING:
	case HASH_LOCK_VERIFYING:
	case HASH_LOCK_UNLOCKING:
		// The lock is busy, and can't be shared yet.
		waitOnHashLock(lock, data_vio);
		break;

	case HASH_LOCK_BYPASSING:
		// Bypass dedupe entirely.
		compressData(data_vio);
		break;

	case HASH_LOCK_DEDUPING:
		launchDedupe(lock, data_vio, false);
		break;

	case HASH_LOCK_DESTROYING:
		// A lock in this state should not be acquired by new VIOs.
		report_bogus_lock_state(lock, data_vio);
		break;

	default:
		report_bogus_lock_state(lock, data_vio);
	}
}

/**********************************************************************/
void continue_hash_lock(struct data_vio *data_vio)
{
	struct hash_lock *lock = data_vio->hashLock;
	// XXX VDOSTORY-190 Eventually we may be able to fold the error handling
	// in at this point instead of using a separate entry point for it.

	switch (lock->state) {
	case HASH_LOCK_WRITING:
		ASSERT_LOG_ONLY(data_vio == lock->agent,
				"only the lock agent may continue the lock");
		finishWriting(lock, data_vio);
		break;

	case HASH_LOCK_DEDUPING:
		finishDeduping(lock, data_vio);
		break;

	case HASH_LOCK_BYPASSING:
		// This data_vio has finished the write path and the lock
		// doesn't need it.
		// XXX This isn't going to be correct if DEDUPING ever uses
		// BYPASSING.
		finishDataVIO(data_vio, VDO_SUCCESS);
		break;

	case HASH_LOCK_INITIALIZING:
	case HASH_LOCK_QUERYING:
	case HASH_LOCK_UPDATING:
	case HASH_LOCK_LOCKING:
	case HASH_LOCK_VERIFYING:
	case HASH_LOCK_UNLOCKING:
	case HASH_LOCK_DESTROYING:
		// A lock in this state should never be re-entered.
		report_bogus_lock_state(lock, data_vio);
		break;

	default:
		report_bogus_lock_state(lock, data_vio);
	}
}

/**********************************************************************/
void continue_hash_lock_on_error(struct data_vio *data_vio)
{
	// XXX We could simply use continue_hash_lock() and check for errors in
	// that.
	abortHashLock(data_vio->hashLock, data_vio);
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
	if (isRingEmpty(&lock->duplicate_ring)) {
		return false;
	}

	struct data_vio *lock_holder =
		data_vio_from_lock_node(lock->duplicate_ring.next);
	bool collides = !compareDataVIOs(lock_holder, candidate);

	if (collides) {
		bump_hash_zone_collision_count(candidate->hashZone);
	} else {
		bump_hash_zone_data_match_count(candidate->hashZone);
	}

	return collides;
}

/**********************************************************************/
static inline int
assert_hash_lock_preconditions(const struct data_vio *data_vio)
{
	int result = ASSERT(data_vio->hashLock == NULL,
			    "must not already hold a hash lock");
	if (result != VDO_SUCCESS) {
		return result;
	}
	result = ASSERT(isRingEmpty(&data_vio->hashLockNode),
			"must not already be a member of a hash lock ring");
	if (result != VDO_SUCCESS) {
		return result;
	}
	return ASSERT(data_vio->recoverySequenceNumber == 0,
		      "must not hold a recovery lock when getting a hash lock");
}

/**********************************************************************/
int acquire_hash_lock(struct data_vio *data_vio)
{
	int result = assert_hash_lock_preconditions(data_vio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct hash_lock *lock;
	result = acquire_hash_lock_from_zone(data_vio->hashZone,
					     &data_vio->chunkName,
					     NULL,
					     &lock);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (is_hash_collision(lock, data_vio)) {
		// Hash collisions are extremely unlikely, but the bogus dedupe
		// would be a data corruption. Bypass dedupe entirely by leaving
		// hashLock unset.
		// XXX clear hashZone too?
		return VDO_SUCCESS;
	}

	setHashLock(data_vio, lock);
	return VDO_SUCCESS;
}

/**********************************************************************/
void release_hash_lock(struct data_vio *data_vio)
{
	struct hash_lock *lock = data_vio->hashLock;
	if (lock == NULL) {
		return;
	}

	setHashLock(data_vio, NULL);

	if (lock->reference_count > 0) {
		// The lock is still in use by other data_vios.
		return;
	}

	set_hash_lock_state(lock, HASH_LOCK_DESTROYING);
	return_hash_lock_to_zone(data_vio->hashZone, &lock);
}

/**
 * Transfer a data_vio's downgraded allocation PBN lock to the data_vio's hash
 * lock, converting it to a duplicate PBN lock.
 *
 * @param data_vio  The data_vio holding the allocation lock to transfer
 **/
static void transfer_allocation_lock(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(data_vio->newMapped.pbn == getDataVIOAllocation(data_vio),
			"transferred lock must be for the block written");

	struct allocating_vio *allocating_vio = dataVIOAsAllocatingVIO(data_vio);
	struct pbn_lock *pbn_lock = allocating_vio->allocation_lock;
	allocating_vio->allocation_lock = NULL;
	allocating_vio->allocation = ZERO_BLOCK;

	ASSERT_LOG_ONLY(is_pbn_read_lock(pbn_lock),
			"must have downgraded the allocation lock before transfer");

	struct hash_lock *hash_lock = data_vio->hashLock;
	hash_lock->duplicate = data_vio->newMapped;
	data_vio->duplicate = data_vio->newMapped;

	// Since the lock is being transferred, the holder count doesn't change
	// (and isn't even safe to examine on this thread).
	hash_lock->duplicate_lock = pbn_lock;
}

/**********************************************************************/
void share_compressed_write_lock(struct data_vio *data_vio,
				 struct pbn_lock *pbn_lock)
{
	ASSERT_LOG_ONLY(get_duplicate_lock(data_vio) == NULL,
			"a duplicate PBN lock should not exist when writing");
	ASSERT_LOG_ONLY(isCompressed(data_vio->newMapped.state),
			"lock transfer must be for a compressed write");
	assertInNewMappedZone(data_vio);

	// First sharer downgrades the lock.
	if (!is_pbn_read_lock(pbn_lock)) {
		downgrade_pbn_write_lock(pbn_lock);
	}

	// Get a share of the PBN lock, ensuring it cannot be released until
	// after this data_vio has had a chance to journal a reference.
	data_vio->duplicate = data_vio->newMapped;
	data_vio->hashLock->duplicate = data_vio->newMapped;
	set_duplicate_lock(data_vio->hashLock, pbn_lock);

	// Claim a reference for this data_vio, which is necessary since another
	// hash_lock might start deduplicating against it before our incRef.
	bool claimed = claim_pbn_lock_increment(pbn_lock);
	ASSERT_LOG_ONLY(claimed,
			"impossible to fail to claim an initial increment");
}
