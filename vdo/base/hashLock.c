/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/hashLock.c#5 $
 */

/**
 * HashLock controls and coordinates writing, index access, and dedupe among
 * groups of DataVIOs concurrently writing identical blocks, allowing them to
 * deduplicate not only against advice but also against each other. This save
 * on index queries and allows those DataVIOs to concurrently deduplicate
 * against a single block instead of being serialized through a PBN read lock.
 * Only one index query is needed for each HashLock, instead of one for every
 * DataVIO.
 *
 * A HashLock acts like a state machine perhaps more than as a lock. Other
 * than the starting and ending states INITIALIZING and DESTROYING, every
 * state represents and is held for the duration of an asynchronous operation.
 * All state transitions are performed on the thread of the HashZone
 * containing the lock. An asynchronous operation is almost always performed
 * upon entering a state, and the callback from that operation triggers
 * exiting the state and entering a new state.
 *
 * In all states except DEDUPING, there is a single DataVIO, called the lock
 * agent, performing the asynchronous operations on behalf of the lock. The
 * agent will change during the lifetime of the lock if the lock is shared by
 * more than one DataVIO. DataVIOs waiting to deduplicate are kept on a wait
 * queue. Viewed a different way, the agent holds the lock exclusively until
 * the lock enters the DEDUPING state, at which point it becomes a shared lock
 * that all the waiters (and any new DataVIOs that arrive) use to share a PBN
 * lock. In state DEDUPING, there is no agent. When the last DataVIO in the
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
 * block (UNLOCKING), and releasing the HashLock itself back to the hash zone
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
 * a PBN for a DataVIO to deduplicate, a new lock is forked and the excess
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
  [HASH_LOCK_BYPASSING]    = "BYPASSING",
  [HASH_LOCK_DEDUPING]     = "DEDUPING",
  [HASH_LOCK_DESTROYING]   = "DESTROYING",
  [HASH_LOCK_INITIALIZING] = "INITIALIZING",
  [HASH_LOCK_LOCKING]      = "LOCKING",
  [HASH_LOCK_QUERYING]     = "QUERYING",
  [HASH_LOCK_UNLOCKING]    = "UNLOCKING",
  [HASH_LOCK_UPDATING]     = "UPDATING",
  [HASH_LOCK_VERIFYING]    = "VERIFYING",
  [HASH_LOCK_WRITING]      = "WRITING",
};

// There are loops in the state diagram, so some forward decl's are needed.
static void startDeduping(HashLock *lock, DataVIO *agent, bool agentIsDone);
static void startLocking(HashLock *lock, DataVIO *agent);
static void startWriting(HashLock *lock, DataVIO *agent);
static void unlockDuplicatePBN(VDOCompletion *completion);
static void transferAllocationLock(DataVIO *dataVIO);

/**********************************************************************/
PBNLock *getDuplicateLock(DataVIO *dataVIO)
{
  if (dataVIO->hashLock == NULL) {
    return NULL;
  }
  return dataVIO->hashLock->duplicateLock;
}

/**********************************************************************/
const char *getHashLockStateName(HashLockState state)
{
  // Catch if a state has been added without updating the name array.
  STATIC_ASSERT((HASH_LOCK_DESTROYING + 1) == COUNT_OF(LOCK_STATE_NAMES));
  return (state < COUNT_OF(LOCK_STATE_NAMES)) ? LOCK_STATE_NAMES[state] : NULL;
}

/**
 * Set the current state of a hash lock.
 *
 * @param lock      The lock to update
 * @param newState  The new state
 **/
static void setHashLockState(HashLock *lock, HashLockState newState)
{
  if (false) {
    logWarning("XXX %" PRIptr " %s -> %s", (void *) lock,
               getHashLockStateName(lock->state),
               getHashLockStateName(newState));
  }
  lock->state = newState;
}

/**
 * Assert that a DataVIO is the agent of its hash lock, and that this is being
 * called in the hash zone.
 *
 * @param dataVIO  The DataVIO expected to be the lock agent
 * @param where    A string describing the function making the assertion
 **/
static void assertHashLockAgent(DataVIO *dataVIO, const char *where)
{
  // Not safe to access the agent field except from the hash zone.
  assertInHashZone(dataVIO);
  ASSERT_LOG_ONLY(dataVIO == dataVIO->hashLock->agent,
                  "%s must be for the hash lock agent", where);
}

/**
 * Set or clear the lock agent.
 *
 * @param lock      The hash lock to update
 * @param newAgent  The new lock agent (may be NULL to clear the agent)
 **/
static void setAgent(HashLock *lock, DataVIO *newAgent)
{
  lock->agent = newAgent;
}

/**
 * Set the duplicate lock held by a hash lock. May only be called in the
 * physical zone of the PBN lock.
 *
 * @param hashLock  The hash lock to update
 * @param pbnLock   The PBN read lock to use as the duplicate lock
 **/
static void setDuplicateLock(HashLock *hashLock, PBNLock *pbnLock)
{
  ASSERT_LOG_ONLY((hashLock->duplicateLock == NULL),
                  "hash lock must not already hold a duplicate lock");

  pbnLock->holderCount += 1;
  hashLock->duplicateLock = pbnLock;
}

/**
 * Convert a pointer to the hashLockNode field in a DataVIO to the enclosing
 * DataVIO.
 *
 * @param lockNode The RingNode to convert
 *
 * @return A pointer to the DataVIO containing the RingNode
 **/
static inline DataVIO *dataVIOFromLockNode(RingNode *lockNode)
{
  return (DataVIO *) ((byte *) lockNode - offsetof(DataVIO, hashLockNode));
}

/**
 * Remove the first DataVIO from the lock's wait queue and return it.
 *
 * @param lock  The lock containing the wait queue
 *
 * @return The first (oldest) waiter in the queue, or <code>NULL</code> if
 *         the queue is empty
 **/
static inline DataVIO *dequeueLockWaiter(HashLock *lock)
{
  return waiterAsDataVIO(dequeueNextWaiter(&lock->waiters));
}

/**
 * Continue processing a DataVIO that has been waiting for an event, setting
 * the result from the event, and continuing in a specified callback function.
 *
 * @param dataVIO   The DataVIO to continue
 * @param result    The current result (will not mask older errors)
 * @param callback  The function in which to continue processing
 **/
static void continueDataVIOIn(DataVIO   *dataVIO,
                              int        result,
                              VDOAction *callback)
{
  dataVIOAsCompletion(dataVIO)->callback = callback;
  continueDataVIO(dataVIO, result);
}

/**
 * Set, change, or clear the hash lock a DataVIO is using. Updates the hash
 * lock (or locks) to reflect the change in membership.
 *
 * @param dataVIO  The DataVIO to update
 * @param newLock  The hash lock the DataVIO is joining
 **/
static void setHashLock(DataVIO *dataVIO, HashLock *newLock)
{
  HashLock *oldLock = dataVIO->hashLock;
  if (oldLock != NULL) {
    ASSERT_LOG_ONLY(dataVIO->hashZone != NULL,
                    "must have a hash zone when halding a hash lock");
    ASSERT_LOG_ONLY(!isRingEmpty(&dataVIO->hashLockNode),
                    "must be on a hash lock ring when holding a hash lock");
    ASSERT_LOG_ONLY(oldLock->referenceCount > 0,
                    "hash lock reference must be counted");

    if ((oldLock->state != HASH_LOCK_BYPASSING)
        && (oldLock->state != HASH_LOCK_UNLOCKING)) {
      // If the reference count goes to zero in a non-terminal state, we're
      // most likely leaking this lock.
      ASSERT_LOG_ONLY(oldLock->referenceCount > 1,
                      "hash locks should only become unreferenced in"
                      " a terminal state, not state %s",
                      getHashLockStateName(oldLock->state));
    }

    unspliceRingNode(&dataVIO->hashLockNode);
    oldLock->referenceCount -= 1;

    dataVIO->hashLock = NULL;
  }

  if (newLock != NULL) {
    // Keep all DataVIOs sharing the lock on a ring since they can complete in
    // any order and we'll always need a pointer to one to compare data.
    pushRingNode(&newLock->duplicateRing, &dataVIO->hashLockNode);
    newLock->referenceCount += 1;

    // XXX Not needed for VDOSTORY-190, but useful for checking whether a test
    // is getting concurrent dedupe, and how much.
    if (newLock->maxReferences < newLock->referenceCount) {
      newLock->maxReferences = newLock->referenceCount;
    }

    dataVIO->hashLock = newLock;
  }
}

/**
 * Bottleneck for DataVIOs that have written or deduplicated and that are no
 * longer needed to be an agent for the hash lock.
 *
 * @param dataVIO  The DataVIO to complete and send to be cleaned up
 **/
static void exitHashLock(DataVIO *dataVIO)
{
  // XXX trace record?

  // Release the hash lock now, saving a thread transition in cleanup.
  releaseHashLock(dataVIO);

  // Complete the DataVIO and start the clean-up path in vioWrite to release
  // any locks it still holds.
  finishDataVIO(dataVIO, VDO_SUCCESS);
}

/**
 * Retire the active lock agent, replacing it with the first lock waiter, and
 * make the retired agent exit the hash lock.
 *
 * @param lock  The hash lock to update
 *
 * @return The new lock agent (which will be NULL if there was no waiter)
 **/
static DataVIO *retireLockAgent(HashLock *lock)
{
  DataVIO *oldAgent = lock->agent;
  DataVIO *newAgent = dequeueLockWaiter(lock);
  setAgent(lock, newAgent);
  exitHashLock(oldAgent);
  if (newAgent != NULL) {
    setDuplicateLocation(newAgent, lock->duplicate);
  }
  return newAgent;
}

/**
 * Callback to call compressData(), putting a DataVIO back on the write path.
 *
 * @param completion  The DataVIO
 **/
static void compressDataCallback(VDOCompletion *completion)
{
  // XXX VDOSTORY-190 need an error check since compressData doesn't have one.
  compressData(asDataVIO(completion));
}

/**
 * Add a DataVIO to the lock's queue of waiters.
 *
 * @param lock     The hash lock on which to wait
 * @param dataVIO  The DataVIO to add to the queue
 **/
static void waitOnHashLock(HashLock *lock, DataVIO *dataVIO)
{
  int result = enqueueDataVIO(&lock->waiters, dataVIO, THIS_LOCATION(NULL));
  if (result != VDO_SUCCESS) {
    // This should be impossible, but if it somehow happens, give up on trying
    // to dedupe the data.
    setHashLock(dataVIO, NULL);
    continueDataVIOIn(dataVIO, result, compressDataCallback);
    return;
  }

  // Make sure the agent doesn't block indefinitely in the packer since it now
  // has at least one other DataVIO waiting on it.
  if ((lock->state == HASH_LOCK_WRITING) && cancelCompression(lock->agent)) {
    /*
     * Even though we're waiting, we also have to send ourselves as a one-way
     * message to the packer to ensure the agent continues executing. This is
     * safe because cancelCompression() guarantees the agent won't continue
     * executing until this message arrives in the packer, and because the
     * wait queue link isn't used for sending the message.
     */
    dataVIO->compression.lockHolder = lock->agent;
    launchPackerCallback(dataVIO, removeLockHolderFromPacker,
                         THIS_LOCATION("$F;cb=removeLockHolderFromPacker"));
  }
}

/**
 * WaiterCallback function that calls compressData on the DataVIO waiter.
 *
 * @param waiter   The DataVIO's waiter link
 * @param context  Not used
 **/
static void compressWaiter(Waiter *waiter,
                           void   *context __attribute__((unused)))
{
  DataVIO *dataVIO     = waiterAsDataVIO(waiter);
  dataVIO->isDuplicate = false;
  compressData(dataVIO);
}

/**
 * Handle the result of the agent for the lock releasing a read lock on
 * duplicate candidate due to aborting the hash lock. This continuation is
 * registered in unlockDuplicatePBN().
 *
 * @param completion  The completion of the DataVIO acting as the lock's agent
 **/
static void finishBypassing(VDOCompletion *completion)
{
  DataVIO  *agent = asDataVIO(completion);
  assertHashLockAgent(agent, __func__);
  HashLock *lock = agent->hashLock;

  ASSERT_LOG_ONLY(lock->duplicateLock == NULL,
                  "must have released the duplicate lock for the hash lock");
  exitHashLock(agent);
}

/**
 * Stop using the hash lock, resuming the old write path for the lock agent
 * and any DataVIOs waiting on it, and put it in a state where DataVIOs
 * entering the lock will use the old dedupe path instead of waiting.
 *
 * @param lock   The hash lock
 * @param agent  The DataVIO acting as the agent for the lock
 **/
static void startBypassing(HashLock *lock, DataVIO *agent)
{
  setHashLockState(lock, HASH_LOCK_BYPASSING);

  // Ensure we don't attempt to update advice when cleaning up.
  lock->updateAdvice = false;

  ASSERT_LOG_ONLY(((agent != NULL) || !hasWaiters(&lock->waiters)),
                  "should not have waiters without an agent");
  notifyAllWaiters(&lock->waiters, compressWaiter, NULL);

  if (lock->duplicateLock != NULL) {
    if (agent != NULL) {
      // The agent must reference the duplicate zone to launch it.
      agent->duplicate = lock->duplicate;
      launchDuplicateZoneCallback(agent, unlockDuplicatePBN,
                                  THIS_LOCATION(NULL));
      return;
    }
    ASSERT_LOG_ONLY(false, "hash lock holding a PBN lock must have an agent");
  }

  if (agent == NULL) {
    return;
  }

  setAgent(lock, NULL);
  agent->isDuplicate = false;
  compressData(agent);
}

/**
 * Abort processing on this hash lock when noticing an error. Currently, this
 * moves the hash lock to the BYPASSING state, to release all pending DataVIOs.
 *
 * @param lock     The HashLock
 * @param dataVIO  The DataVIO with the error
 **/
static void abortHashLock(HashLock *lock, DataVIO *dataVIO)
{
  // If we've already aborted the lock, don't try to re-abort it; just exit.
  if (lock->state == HASH_LOCK_BYPASSING) {
    exitHashLock(dataVIO);
    return;
  }

  if (dataVIO != lock->agent) {
    if ((lock->agent != NULL) || (lock->referenceCount > 1)) {
      // Other DataVIOs are still sharing the lock (which should be DEDUPING),
      // so just kick this one out of the lock to report its error.
      ASSERT_LOG_ONLY(lock->agent == NULL,
                      "only active agent should call abortHashLock");
      exitHashLock(dataVIO);
      return;
    }
    // Make the lone DataVIO the lock agent so it can abort and clean up.
    setAgent(lock, dataVIO);
  }

  startBypassing(lock, dataVIO);
}

/**
 * Handle the result of the agent for the lock releasing a read lock on
 * duplicate candidate. This continuation is registered in
 * unlockDuplicatePBN().
 *
 * @param completion  The completion of the DataVIO acting as the lock's agent
 **/
static void finishUnlocking(VDOCompletion *completion)
{
  DataVIO  *agent = asDataVIO(completion);
  assertHashLockAgent(agent, __func__);
  HashLock *lock = agent->hashLock;

  ASSERT_LOG_ONLY(lock->duplicateLock == NULL,
                  "must have released the duplicate lock for the hash lock");

  if (completion->result != VDO_SUCCESS) {
    abortHashLock(lock, agent);
    return;
  }

  if (!lock->verified) {
    /*
     * UNLOCKING -> WRITING transition: The lock we released was on an
     * unverified block, so it must have been a lock on advice we were
     * verifying, not on a location that was used for deduplication. Go write
     * (or compress) the block to get a location to dedupe against.
     */
    startWriting(lock, agent);
    return;
  }

  // With the lock released, the verified duplicate block may already have
  // changed and will need to be re-verified if a waiter arrived.
  lock->verified = false;

  if (hasWaiters(&lock->waiters)) {
    /*
     * UNLOCKING -> LOCKING transition: A new DataVIO entered the hash lock
     * while the agent was releasing the PBN lock. The current agent exits and
     * the waiter has to re-lock and re-verify the duplicate location.
     */
    // XXX VDOSTORY-190 If we used the current agent to re-acquire the PBN
    // lock we wouldn't need to re-verify.
    agent = retireLockAgent(lock);
    startLocking(lock, agent);
    return;
  }

  /*
   * UNLOCKING -> DESTROYING transition: The agent is done with the lock
   * and no other DataVIOs reference it, so remove it from the lock map
   * and return it to the pool.
   */
  exitHashLock(agent);
}

/**
 * Release a read lock on the PBN of the block that may or may not have
 * contained duplicate data. This continuation is launched by
 * startUnlocking(), and calls back to finishUnlocking() on the hash zone
 * thread.
 *
 * @param completion  The completion of the DataVIO acting as the lock's agent
 **/
static void unlockDuplicatePBN(VDOCompletion *completion)
{
  DataVIO *agent = asDataVIO(completion);
  assertInDuplicateZone(agent);
  HashLock *lock = agent->hashLock;

  ASSERT_LOG_ONLY(lock->duplicateLock != NULL,
                  "must have a duplicate lock to release");

  releasePBNLock(agent->duplicate.zone, agent->duplicate.pbn,
                 &lock->duplicateLock);

  if (lock->state == HASH_LOCK_BYPASSING) {
    launchHashZoneCallback(agent, finishBypassing, THIS_LOCATION(NULL));
  } else {
    launchHashZoneCallback(agent, finishUnlocking, THIS_LOCATION(NULL));
  }
}

/**
 * Release a read lock on the PBN of the block that may or may not have
 * contained duplicate data.
 *
 * @param lock   The hash lock
 * @param agent  The DataVIO currently acting as the agent for the lock
 **/
static void startUnlocking(HashLock *lock, DataVIO *agent)
{
  setHashLockState(lock, HASH_LOCK_UNLOCKING);

  /*
   * XXX If we arrange to continue on the duplicate zone thread when
   * verification fails, and don't explicitly change lock states (or use an
   * agent-local state, or an atomic), we can avoid a thread transition here.
   */
  launchDuplicateZoneCallback(agent, unlockDuplicatePBN, THIS_LOCATION(NULL));
}

/**
 * Process the result of a UDS update performed by the agent for the lock.
 * This continuation is registered in startQuerying().
 *
 * @param completion  The completion of the DataVIO that performed the update
 **/
static void finishUpdating(VDOCompletion *completion)
{
  DataVIO  *agent = asDataVIO(completion);
  assertHashLockAgent(agent, __func__);
  HashLock *lock = agent->hashLock;

  if (completion->result != VDO_SUCCESS) {
    abortHashLock(lock, agent);
    return;
  }

  // UDS was updated successfully, so don't update again unless the
  // duplicate location changes due to rollover.
  lock->updateAdvice = false;

  if (hasWaiters(&lock->waiters)) {
    /*
     * UPDATING -> DEDUPING transition: A new DataVIO arrived during the UDS
     * update. Send it on the verified dedupe path. The agent is done with the
     * lock, but the lock may still need to use it to clean up after rollover.
     */
    startDeduping(lock, agent, true);
    return;
  }

  if (lock->duplicateLock != NULL) {
    /*
     * UPDATING -> UNLOCKING transition: No one is waiting to dedupe, but we
     * hold a duplicate PBN lock, so go release it.
     */
    startUnlocking(lock, agent);
  } else {
    /*
     * UPDATING -> DESTROYING transition: No one is waiting to dedupe and
     * there's no lock to release.
     */
    // XXX startDestroying(lock, agent);
    startBypassing(lock, NULL);
    exitHashLock(agent);
  }
}

/**
 * Continue deduplication with the last step, updating UDS with the location
 * of the duplicate that should be returned as advice in the future.
 *
 * @param lock   The hash lock
 * @param agent  The DataVIO currently acting as the agent for the lock
 **/
static void startUpdating(HashLock *lock, DataVIO *agent)
{
  setHashLockState(lock, HASH_LOCK_UPDATING);

  ASSERT_LOG_ONLY(lock->verified, "new advice should have been verified");
  ASSERT_LOG_ONLY(lock->updateAdvice, "should only update advice if needed");

  agent->lastAsyncOperation = UPDATE_INDEX;
  setHashZoneCallback(agent, finishUpdating, THIS_LOCATION(NULL));
  dataVIOAsCompletion(agent)->layer->updateAlbireo(agent);
}

/**
 * Handle a DataVIO that has finished deduplicating against the block locked
 * by the hash lock. If there are other DataVIOs still sharing the lock, this
 * will just release the DataVIO's share of the lock and finish processing the
 * DataVIO. If this is the last DataVIO holding the lock, this makes the
 * DataVIO the lock agent and uses it to advance the state of the lock so it
 * can eventually be released.
 *
 * @param lock     The hash lock
 * @param dataVIO  The lock holder that has finished deduplicating
 **/
static void finishDeduping(HashLock *lock, DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(lock->agent == NULL, "shouldn't have an agent in DEDUPING");
  ASSERT_LOG_ONLY(!hasWaiters(&lock->waiters),
                  "shouldn't have any lock waiters in DEDUPING");

  // Just release the lock reference if other DataVIOs are still deduping.
  if (lock->referenceCount > 1) {
    exitHashLock(dataVIO);
    return;
  }

  // The hash lock must have an agent for all other lock states.
  DataVIO *agent = dataVIO;
  setAgent(lock, agent);

  if (lock->updateAdvice) {
    /*
     * DEDUPING -> UPDATING transition: The location of the duplicate block
     * changed since the initial UDS query because of compression, rollover,
     * or because the query agent didn't have an allocation. The UDS update
     * was delayed in case there was another change in location, but with only
     * this DataVIO using the hash lock, it's time to update the advice.
     */
    startUpdating(lock, agent);
  } else {
    /*
     * DEDUPING -> UNLOCKING transition: Release the PBN read lock on the
     * duplicate location so the hash lock itself can be released (contingent
     * on no new DataVIOs arriving in the lock before the agent returns).
     */
    startUnlocking(lock, agent);
  }
}

/**
 * Implements WaiterCallback. Binds the DataVIO that was waiting to a new hash
 * lock and waits on that lock.
 **/
static void enterForkedLock(Waiter *waiter, void *context)
{
  DataVIO  *dataVIO = waiterAsDataVIO(waiter);
  HashLock *newLock = (HashLock *) context;

  setHashLock(dataVIO, newLock);
  waitOnHashLock(newLock, dataVIO);
}

/**
 * Fork a hash lock because it has run out of increments on the duplicate PBN.
 * Transfers the new agent and any lock waiters to a new hash lock instance
 * which takes the place of the old lock in the lock map. The old lock remains
 * active, but will not update advice.
 *
 * @param oldLock   The hash lock to fork
 * @param newAgent  The DataVIO that will be the agent for the new lock
 **/
static void forkHashLock(HashLock *oldLock, DataVIO *newAgent)
{
  HashLock *newLock;
  int result = acquireHashLockFromZone(newAgent->hashZone,
                                       &newAgent->chunkName,
                                       oldLock, &newLock);
  if (result != VDO_SUCCESS) {
    abortHashLock(oldLock, newAgent);
    return;
  }

  // Only one of the two locks should update UDS. The old lock is out of
  // references, so it would be poor dedupe advice in the short term.
  oldLock->updateAdvice = false;
  newLock->updateAdvice = true;

  setHashLock(newAgent, newLock);
  setAgent(newLock, newAgent);

  notifyAllWaiters(&oldLock->waiters, enterForkedLock, newLock);

  newAgent->isDuplicate = false;
  startWriting(newLock, newAgent);
}

/**
 * Reserve a reference count increment for a DataVIO and launch it on the
 * dedupe path. If no increments are available, this will roll over to a new
 * hash lock and launch the DataVIO as the writing agent for that lock.
 *
 * @param lock      The hash lock
 * @param dataVIO   The DataVIO to deduplicate using the hash lock
 * @param hasClaim  <code>true</code> if the dataVIO already has claimed
 *                  an increment from the duplicate lock
 **/
static void launchDedupe(HashLock *lock, DataVIO *dataVIO, bool hasClaim)
{
  if (!hasClaim && !claimPBNLockIncrement(lock->duplicateLock)) {
    // Out of increments, so must roll over to a new lock.
    forkHashLock(lock, dataVIO);
    return;
  }

  // Deduplicate against the lock's verified location.
  setDuplicateLocation(dataVIO, lock->duplicate);
  launchDuplicateZoneCallback(dataVIO, shareBlock,
                              THIS_LOCATION("$F;cb=shareBlock"));
}

/**
 * Enter the hash lock state where DataVIOs deduplicate in parallel against a
 * true copy of their data on disk. If the agent itself needs to deduplicate,
 * an increment for it must already have been claimed from the duplicate lock,
 * ensuring the hash lock will still have a DataVIO holding it.
 *
 * @param lock         The hash lock
 * @param agent        The DataVIO acting as the agent for the lock
 * @param agentIsDone  <code>true</code> only if the agent has already written
 *                     or deduplicated against its data
 **/
static void startDeduping(HashLock *lock, DataVIO *agent, bool agentIsDone)
{
  setHashLockState(lock, HASH_LOCK_DEDUPING);

  // We don't take the downgraded allocation lock from the agent unless we
  // actually need to deduplicate against it.
  if (lock->duplicateLock == NULL) {
    ASSERT_LOG_ONLY(!isCompressed(agent->newMapped.state),
                    "compression must have shared a lock");
    ASSERT_LOG_ONLY(agentIsDone, "agent must have written the new duplicate");
    transferAllocationLock(agent);
  }

  ASSERT_LOG_ONLY(isPBNReadLock(lock->duplicateLock),
                  "duplicateLock must be a PBN read lock");

  /*
   * This state is not like any of the other states. There is no designated
   * agent--the agent transitioning to this state and all the waiters will be
   * launched to deduplicate in parallel.
   */
  setAgent(lock, NULL);

  /*
   * Launch the agent (if not already deduplicated) and as many lock waiters
   * as we have available increments for on the dedupe path. If we run out of
   * increments, rollover will be triggered and the remaining waiters will be
   * transferred to the new lock.
   */
  if (!agentIsDone) {
    launchDedupe(lock, agent, true);
    agent = NULL;
  }
  while (hasWaiters(&lock->waiters)) {
    launchDedupe(lock, dequeueLockWaiter(lock), false);
  }

  if (agentIsDone) {
    /*
     * In the degenerate case where all the waiters rolled over to a new lock,
     * this will continue to use the old agent to clean up this lock, and
     * otherwise it just lets the agent exit the lock.
     */
    finishDeduping(lock, agent);
  }
}

/**
 * Handle the result of the agent for the lock comparing its data to the
 * duplicate candidate. This continuation is registered in startVerifying().
 *
 * @param completion  The completion of the DataVIO used to verify dedupe
 **/
static void finishVerifying(VDOCompletion *completion)
{
  DataVIO  *agent = asDataVIO(completion);
  assertHashLockAgent(agent, __func__);
  HashLock *lock = agent->hashLock;

  if (completion->result != VDO_SUCCESS) {
    // XXX VDOSTORY-190 should convert verify IO errors to verification failure
    abortHashLock(lock, agent);
    return;
  }

  lock->verified = agent->isDuplicate;

  // Only count the result of the initial verification of the advice as valid
  // or stale, and not any re-verifications due to PBN lock releases.
  if (!lock->verifyCounted) {
    lock->verifyCounted = true;
    if (lock->verified) {
      bumpHashZoneValidAdviceCount(agent->hashZone);
    } else {
      bumpHashZoneStaleAdviceCount(agent->hashZone);
    }
  }

  // Even if the block is a verified duplicate, we can't start to deduplicate
  // unless we can claim a reference count increment for the agent.
  if (lock->verified && !claimPBNLockIncrement(lock->duplicateLock)) {
    agent->isDuplicate = false;
    lock->verified     = false;
  }

  if (lock->verified) {
    /*
     * VERIFYING -> DEDUPING transition: The advice is for a true duplicate,
     * so start deduplicating against it, if references are available.
     */
    startDeduping(lock, agent, false);
  } else {
    /*
     * VERIFYING -> UNLOCKING transition: Either the verify failed or we'd try
     * to dedupe and roll over immediately, which would fail because it would
     * leave the lock without an agent to release the PBN lock. In both cases,
     * the data will have to be written or compressed, but first the advice
     * PBN must be unlocked by the VERIFYING agent.
     */
    lock->updateAdvice = true;
    startUnlocking(lock, agent);
  }
}

/**
 * Continue the deduplication path for a hash lock by using the agent to read
 * (and possibly decompress) the data at the candidate duplicate location,
 * comparing it to the data in the agent to verify that the candidate is
 * identical to all the DataVIOs sharing the hash. If so, it can be
 * deduplicated against, otherwise a DataVIO allocation will have to be
 * written to and used for dedupe.
 *
 * @param lock   The hash lock (must be LOCKING)
 * @param agent  The DataVIO to use to read and compare candidate data
 **/
static void startVerifying(HashLock *lock, DataVIO *agent)
{
  setHashLockState(lock, HASH_LOCK_VERIFYING);
  ASSERT_LOG_ONLY(!lock->verified, "hash lock only verifies advice once");

  /*
   * XXX VDOSTORY-190 Optimization: This is one of those places where the zone
   * and continuation we want to use depends on the outcome of the comparison.
   * If we could choose which path in the layer thread before continuing, we
   * could save a thread transition in one of the two cases (assuming we're
   * willing to delay visibility of the the hash lock state change).
   */
  VDOCompletion *completion = dataVIOAsCompletion(agent);
  agent->lastAsyncOperation = VERIFY_DEDUPLICATION;
  setHashZoneCallback(agent, finishVerifying, THIS_LOCATION(NULL));
  completion->layer->verifyDuplication(agent);
}

/**
 * Handle the result of the agent for the lock attempting to obtain a PBN read
 * lock on the candidate duplicate block. this continuation is registered in
 * lockDuplicatePBN().
 *
 * @param completion  The completion of the DataVIO that attempted to get
 *                    the read lock
 **/
static void finishLocking(VDOCompletion *completion)
{
  DataVIO  *agent = asDataVIO(completion);
  assertHashLockAgent(agent, __func__);
  HashLock *lock = agent->hashLock;

  if (completion->result != VDO_SUCCESS) {
    // XXX clearDuplicateLocation()?
    agent->isDuplicate = false;
    abortHashLock(lock, agent);
    return;
  }

  if (!agent->isDuplicate) {
    ASSERT_LOG_ONLY(lock->duplicateLock == NULL,
                  "must not hold duplicateLock if not flagged as a duplicate");
    /*
     * LOCKING -> WRITING transition: The advice block is being modified or
     * has no available references, so try to write or compress the data,
     * remembering to update UDS later with the new advice.
     */
    bumpHashZoneStaleAdviceCount(agent->hashZone);
    lock->updateAdvice = true;
    startWriting(lock, agent);
    return;
  }

  ASSERT_LOG_ONLY(lock->duplicateLock != NULL,
                  "must hold duplicateLock if flagged as a duplicate");

  if (!lock->verified) {
    /*
     * LOCKING -> VERIFYING transition: Continue on the unverified dedupe path,
     * reading the candidate duplicate and comparing it to the agent's data to
     * decide whether it is a true duplicate or stale advice.
     */
    startVerifying(lock, agent);
    return;
  }

  if (!claimPBNLockIncrement(lock->duplicateLock)) {
    /*
     * LOCKING -> UNLOCKING transition: The verified block was re-locked, but
     * has no available increments left. Must first release the useless PBN
     * read lock before rolling over to a new copy of the block.
     */
    agent->isDuplicate = false;
    lock->verified     = false;
    lock->updateAdvice = true;
    startUnlocking(lock, agent);
    return;
  }

  /*
   * LOCKING -> DEDUPING transition: Continue on the verified dedupe path,
   * deduplicating against a location that was previously verified or
   * written to.
   */
  startDeduping(lock, agent, false);
}

/**
 * Acquire a read lock on the PBN of the block containing candidate duplicate
 * data (compressed or uncompressed). If the PBN is already locked for
 * writing, the lock attempt is abandoned and isDuplicate will be cleared
 * before calling back. this continuation is launched from startLocking(), and
 * calls back to finishLocking() on the hash zone thread.
 *
 * @param completion The completion of the DataVIO attempting to acquire the
 *                   physical block lock on behalf of its hash lock
 **/
static void lockDuplicatePBN(VDOCompletion *completion)
{
  DataVIO      *agent = asDataVIO(completion);
  PhysicalZone *zone  = agent->duplicate.zone;
  assertInDuplicateZone(agent);

  setHashZoneCallback(agent, finishLocking, THIS_LOCATION(NULL));

  // While in the zone that owns it, find out how many additional references
  // can be made to the block if it turns out to truly be a duplicate.
  SlabDepot *depot = getSlabDepot(getVDOFromDataVIO(agent));
  unsigned int incrementLimit = getIncrementLimit(depot, agent->duplicate.pbn);
  if (incrementLimit == 0) {
    // We could deduplicate against it later if a reference happened to be
    // released during verification, but it's probably better to bail out now.
    // XXX clearDuplicateLocation()?
    agent->isDuplicate = false;
    continueDataVIO(agent, VDO_SUCCESS);
    return;
  }

  PBNLock *lock;
  int result = attemptPBNLock(zone, agent->duplicate.pbn, VIO_READ_LOCK,
                              &lock);
  if (result != VDO_SUCCESS) {
    continueDataVIO(agent, result);
    return;
  }

  if (!isPBNReadLock(lock)) {
    /*
     * There are three cases of write locks: uncompressed data block writes,
     * compressed (packed) block writes, and block map page writes. In all
     * three cases, we give up on trying to verify the advice and don't bother
     * to try deduplicate against the data in the write lock holder.
     *
     * 1) We don't ever want to try to deduplicate against a block map page.
     *
     * 2a) It's very unlikely we'd deduplicate against an entire packed block,
     * both because of the chance of matching it, and because we don't record
     * advice for it, but for the uncompressed representation of all the
     * fragments it contains. The only way we'd be getting lock contention is
     * if we've written the same representation coincidentally before, had it
     * become unreferenced, and it just happened to be packed together from
     * compressed writes when we go to verify the lucky advice. Giving up is a
     * miniscule loss of potential dedupe.
     *
     * 2b) If the advice is for a slot of a compressed block, it's about to
     * get smashed, and the write smashing it cannot contain our data--it
     * would have to be writing on behalf of our hash lock, but that's
     * impossible since we're the lock agent.
     *
     * 3a) If the lock is held by a DataVIO with different data, the advice is
     * already stale or is about to become stale.
     *
     * 3b) If the lock is held by a DataVIO that matches us, we may as well
     * either write it ourselves (or reference the copy we already wrote)
     * instead of potentially having many duplicates wait for the lock holder
     * to write, journal, hash, and finally arrive in the hash lock. All we
     * lose is a chance to avoid a UDS update in the very rare case of advice
     * for a free block that just happened to be allocated to a DataVIO with
     * the same hash. In async mode, there's also a chance to save on a block
     * write, at the cost of a block verify. Saving on a full block compare in
     * all stale advice cases almost certainly outweighs saving a UDS update
     * in a lucky case where advice would have been saved from becoming stale.
     */
    // XXX clearDuplicateLocation()?
    agent->isDuplicate = false;
    continueDataVIO(agent, VDO_SUCCESS);
    return;
  }

  if (lock->holderCount == 0) {
    // Ensure that the newly-locked block is referenced.
    Slab *slab = getSlab(depot, agent->duplicate.pbn);
    result = acquireProvisionalReference(slab, agent->duplicate.pbn, lock);
    if (result != VDO_SUCCESS) {
      logWarningWithStringError(result,
                                "Error acquiring provisional reference for "
                                "dedupe candidate; aborting dedupe");
      agent->isDuplicate = false;
      releasePBNLock(zone, agent->duplicate.pbn, &lock);
      continueDataVIO(agent, result);
      return;
    }

    /*
     * The increment limit we grabbed earlier is still valid. The lock now
     * holds the rights to acquire all those references. Those rights will be
     * claimed by hash locks sharing this read lock.
     */
    lock->incrementLimit = incrementLimit;
  }

  // We've successfully acquired a read lock on behalf of the hash lock,
  // so mark it as such.
  setDuplicateLock(agent->hashLock, lock);

  /*
   * XXX VDOSTORY-190 Optimization: Same as startLocking() lazily changing
   * state to save on having to switch back to the hash zone thread. Here we
   * could directly launch the block verify, then switch to a hash thread.
   */
  continueDataVIO(agent, VDO_SUCCESS);
}

/**
 * Continue deduplication for a hash lock that has obtained valid advice
 * of a potential duplicate through its agent.
 *
 * @param lock   The hash lock (currently must be QUERYING)
 * @param agent  The DataVIO bearing the dedupe advice
 **/
static void startLocking(HashLock *lock, DataVIO *agent)
{
  ASSERT_LOG_ONLY(lock->duplicateLock == NULL,
                  "must not acquire a duplicate lock when already holding it");

  setHashLockState(lock, HASH_LOCK_LOCKING);

  /*
   * XXX VDOSTORY-190 Optimization: If we arrange to continue on the duplicate
   * zone thread when accepting the advice, and don't explicitly change lock
   * states (or use an agent-local state, or an atomic), we can avoid a thread
   * transition here.
   */
  agent->lastAsyncOperation = ACQUIRE_PBN_READ_LOCK;
  launchDuplicateZoneCallback(agent, lockDuplicatePBN, THIS_LOCATION(NULL));
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
 * @param agent  The DataVIO that wrote its data for the lock
 **/
static void finishWriting(HashLock *lock, DataVIO *agent)
{
  // Dedupe against the data block or compressed block slot the agent wrote.
  // Since we know the write succeeded, there's no need to verify it.
  lock->duplicate = agent->newMapped;
  lock->verified  = true;

  if (isCompressed(lock->duplicate.state) && lock->registered) {
    // Compression means the location we gave in the UDS query is not the
    // location we're using to deduplicate.
    lock->updateAdvice = true;
  }

  // If there are any waiters, we need to start deduping them.
  if (hasWaiters(&lock->waiters)) {
    /*
     * WRITING -> DEDUPING transition: an asynchronously-written block
     * failed to compress, so the PBN lock on the written copy was already
     * transferred. The agent is done with the lock, but the lock may
     * still need to use it to clean up after rollover.
     */
    startDeduping(lock, agent, true);
    return;
  }

  // There are no waiters and the agent has successfully written, so take a
  // step towards being able to release the hash lock (or just release it).
  if (lock->updateAdvice) {
    /*
     * WRITING -> UPDATING transition: There's no waiter and a UDS update is
     * needed, so retain the WRITING agent and use it to launch the update.
     * The happens on compression, rollover, or the QUERYING agent not having
     * an allocation.
     */
    startUpdating(lock, agent);
  } else if (lock->duplicateLock != NULL) {
    /*
     * WRITING -> UNLOCKING transition: There's no waiter and no update
     * needed, but the compressed write gave us a shared duplicate lock that
     * we must release.
     */
    setDuplicateLocation(agent, lock->duplicate);
    startUnlocking(lock, agent);
  } else {
    /*
     * WRITING -> DESTROYING transition: There's no waiter, no update needed,
     * and no duplicate lock held, so both the agent and lock have no more
     * work to do. The agent will release its allocation lock in cleanup.
     */
    // XXX startDestroying(lock, agent);
    startBypassing(lock, NULL);
    exitHashLock(agent);
  }
}

/**
 * Search through the lock waiters for a DataVIO that has an allocation. If
 * one is found, swap agents, put the old agent at the head of the wait queue,
 * then return the new agent. Otherwise, just return the current agent.
 *
 * @param lock   The hash lock to modify
 **/
static DataVIO *selectWritingAgent(HashLock *lock)
{
  // This should-be-impossible condition is the only cause for
  // enqueueDataVIO() to fail later on, where it would be a pain to handle.
  int result = ASSERT(!isWaiting(dataVIOAsWaiter(lock->agent)),
                      "agent must not be waiting");
  if (result != VDO_SUCCESS) {
    return lock->agent;
  }

  WaitQueue tempQueue;
  initializeWaitQueue(&tempQueue);

  // Move waiters to the temp queue one-by-one until we find an allocation.
  // Not ideal to search, but it only happens when nearly out of space.
  DataVIO *dataVIO;
  while (((dataVIO = dequeueLockWaiter(lock)) != NULL)
         && !hasAllocation(dataVIO)) {
    // Use the lower-level enqueue since we're just moving waiters around.
    int result = enqueueWaiter(&tempQueue, dataVIOAsWaiter(dataVIO));
    // The only error is the DataVIO already being on a wait queue, and since
    // we just dequeued it, that could only happen due to a memory smash or
    // concurrent use of that DataVIO.
    ASSERT_LOG_ONLY(result == VDO_SUCCESS, "impossible enqueueWaiter error");
  }

  if (dataVIO != NULL) {
    // Move the rest of the waiters over to the temp queue, preserving the
    // order they arrived at the lock.
    transferAllWaiters(&lock->waiters, &tempQueue);

    // The current agent is being replaced and will have to wait to dedupe;
    // make it the first waiter since it was the first to reach the lock.
    int result = enqueueDataVIO(&lock->waiters, lock->agent,
                                THIS_LOCATION(NULL));
    ASSERT_LOG_ONLY(result == VDO_SUCCESS,
                    "impossible enqueueDataVIO error after isWaiting checked");
    setAgent(lock, dataVIO);
  } else {
    // No one has an allocation, so keep the current agent.
    dataVIO = lock->agent;
  }

  // Swap all the waiters back onto the lock's queue.
  transferAllWaiters(&tempQueue, &lock->waiters);
  return dataVIO;
}

/**
 * Begin the non-duplicate write path for a hash lock that had no advice,
 * selecting a DataVIO with an allocation as a new agent, if necessary,
 * then resuming the agent on the DataVIO write path.
 *
 * @param lock   The hash lock (currently must be QUERYING)
 * @param agent  The DataVIO currently acting as the agent for the lock
 **/
static void startWriting(HashLock *lock, DataVIO *agent)
{
  setHashLockState(lock, HASH_LOCK_WRITING);

  // The agent might not have received an allocation and so can't be used for
  // writing, but it's entirely possible that one of the waiters did.
  if (!hasAllocation(agent)) {
    agent = selectWritingAgent(lock);
    // If none of the waiters had an allocation, the writes all have to fail.
    if (!hasAllocation(agent)) {
      /*
       * XXX VDOSTORY-190 Should we keep a variant of BYPASSING that causes
       * new arrivals to fail immediately if they don't have an allocation? It
       * might be possible that on some path there would be non-waiters still
       * referencing the lock, so it would remain in the map as everything is
       * currently spelled, even if the agent and all the waiters release.
       */
      startBypassing(lock, agent);
      return;
    }
  }

  // If the agent compresses, it might wait indefinitely in the packer,
  // which would be bad if there are any other DataVIOs waiting.
  if (hasWaiters(&lock->waiters)) {
    // XXX in sync mode, transition directly to LOCKING to start dedupe?
    cancelCompression(agent);
  }

  /*
   * Send the agent to the compress/pack/async-write path in vioWrite. If it
   * succeeds, it will return to the hash lock via continueHashLock() and call
   * finishWriting().
   */
  compressData(agent);
}

/**
 * Process the result of a UDS query performed by the agent for the lock. This
 * continuation is registered in startQuerying().
 *
 * @param completion  The completion of the DataVIO that performed the query
 **/
static void finishQuerying(VDOCompletion *completion)
{
  DataVIO  *agent = asDataVIO(completion);
  assertHashLockAgent(agent, __func__);
  HashLock *lock = agent->hashLock;

  if (completion->result != VDO_SUCCESS) {
    abortHashLock(lock, agent);
    return;
  }

  if (agent->isDuplicate) {
    lock->duplicate = agent->duplicate;
    /*
     * QUERYING -> LOCKING transition: Valid advice was obtained from UDS.
     * Use the QUERYING agent to start the hash lock on the unverified dedupe
     * path, verifying that the advice can be used.
     */
    startLocking(lock, agent);
  } else {
    // The agent will be used as the duplicate if has an allocation; if it
    // does, that location was posted to UDS, so no update will be needed.
    lock->updateAdvice = !hasAllocation(agent);
    /*
     * QUERYING -> WRITING transition: There was no advice or the advice
     * wasn't valid, so try to write or compress the data.
     */
    startWriting(lock, agent);
  }
}

/**
 * Start deduplication for a hash lock that has finished initializing by
 * making the DataVIO that requested it the agent, entering the QUERYING
 * state, and using the agent to perform the UDS query on behalf of the lock.
 *
 * @param lock     The initialized hash lock
 * @param dataVIO  The DataVIO that has just obtained the new lock
 **/
static void startQuerying(HashLock *lock, DataVIO *dataVIO)
{
  setAgent(lock, dataVIO);
  setHashLockState(lock, HASH_LOCK_QUERYING);

  VDOCompletion *completion   = dataVIOAsCompletion(dataVIO);
  dataVIO->lastAsyncOperation = CHECK_FOR_DEDUPLICATION;
  setHashZoneCallback(dataVIO, finishQuerying, THIS_LOCATION(NULL));
  completion->layer->checkForDuplication(dataVIO);
}

/**
 * Complain that a DataVIO has entered a HashLock that is in an unimplemented
 * or unusable state and continue the DataVIO with an error.
 *
 * @param lock     The hash lock
 * @param dataVIO  The DataVIO attempting to enter the lock
 **/
static void reportBogusLockState(HashLock *lock, DataVIO *dataVIO)
{
  int result = ASSERT_FALSE("hash lock must not be in unimplemented state %s",
                            getHashLockStateName(lock->state));
  continueDataVIOIn(dataVIO, result, compressDataCallback);
}

/**********************************************************************/
void enterHashLock(DataVIO *dataVIO)
{
  HashLock *lock = dataVIO->hashLock;
  switch (lock->state) {
  case HASH_LOCK_INITIALIZING:
    startQuerying(lock, dataVIO);
    break;

  case HASH_LOCK_QUERYING:
  case HASH_LOCK_WRITING:
  case HASH_LOCK_UPDATING:
  case HASH_LOCK_LOCKING:
  case HASH_LOCK_VERIFYING:
  case HASH_LOCK_UNLOCKING:
    // The lock is busy, and can't be shared yet.
    waitOnHashLock(lock, dataVIO);
    break;

  case HASH_LOCK_BYPASSING:
    // Bypass dedupe entirely.
    compressData(dataVIO);
    break;

  case HASH_LOCK_DEDUPING:
    launchDedupe(lock, dataVIO, false);
    break;

  case HASH_LOCK_DESTROYING:
    // A lock in this state should not be acquired by new VIOs.
    reportBogusLockState(lock, dataVIO);
    break;

  default:
    reportBogusLockState(lock, dataVIO);
  }
}

/**********************************************************************/
void continueHashLock(DataVIO *dataVIO)
{
  HashLock *lock = dataVIO->hashLock;
  // XXX VDOSTORY-190 Eventually we may be able to fold the error handling
  // in at this point instead of using a separate entry point for it.

  switch (lock->state) {
  case HASH_LOCK_WRITING:
    ASSERT_LOG_ONLY(dataVIO == lock->agent,
                    "only the lock agent may continue the lock");
    finishWriting(lock, dataVIO);
    break;

  case HASH_LOCK_DEDUPING:
    finishDeduping(lock, dataVIO);
    break;

  case HASH_LOCK_BYPASSING:
    // This DataVIO has finished the write path and the lock doesn't need it.
    // XXX This isn't going to be correct if DEDUPING ever uses BYPASSING.
    finishDataVIO(dataVIO, VDO_SUCCESS);
    break;

  case HASH_LOCK_INITIALIZING:
  case HASH_LOCK_QUERYING:
  case HASH_LOCK_UPDATING:
  case HASH_LOCK_LOCKING:
  case HASH_LOCK_VERIFYING:
  case HASH_LOCK_UNLOCKING:
  case HASH_LOCK_DESTROYING:
    // A lock in this state should never be re-entered.
    reportBogusLockState(lock, dataVIO);
    break;

  default:
    reportBogusLockState(lock, dataVIO);
  }
}

/**********************************************************************/
void continueHashLockOnError(DataVIO *dataVIO)
{
  // XXX We could simply use continueHashLock() and check for errors in that.
  abortHashLock(dataVIO->hashLock, dataVIO);
}

/**
 * Check whether the data in DataVIOs sharing a lock is different than in a
 * DataVIO seeking to share the lock, which should only be possible in the
 * extremely unlikely case of a hash collision.
 *
 * @param lock       The lock to check
 * @param candidate  The DataVIO seeking to share the lock
 *
 * @return <code>true</code> if the given DataVIO must not share the lock
 *         because it doesn't have the same data as the lock holders
 **/
static bool isHashCollision(HashLock *lock, DataVIO *candidate)
{
  if (isRingEmpty(&lock->duplicateRing)) {
    return false;
  }

  DataVIO       *lockHolder = dataVIOFromLockNode(lock->duplicateRing.next);
  PhysicalLayer *layer      = dataVIOAsCompletion(candidate)->layer;
  bool           collides   = !layer->compareDataVIOs(lockHolder, candidate);

  if (collides) {
    bumpHashZoneCollisionCount(candidate->hashZone);
  } else {
    bumpHashZoneDataMatchCount(candidate->hashZone);
  }

  return collides;
}

/**********************************************************************/
static inline int assertHashLockPreconditions(const DataVIO *dataVIO)
{
  int result = ASSERT(dataVIO->hashLock == NULL,
                      "must not already hold a hash lock");
  if (result != VDO_SUCCESS) {
    return result;
  }
  result = ASSERT(isRingEmpty(&dataVIO->hashLockNode),
                  "must not already be a member of a hash lock ring");
  if (result != VDO_SUCCESS) {
    return result;
  }
  return ASSERT(dataVIO->recoverySequenceNumber == 0,
                "must not hold a recovery lock when getting a hash lock");
}

/**********************************************************************/
int acquireHashLock(DataVIO *dataVIO)
{
  int result = assertHashLockPreconditions(dataVIO);
  if (result != VDO_SUCCESS) {
    return result;
  }

  HashLock *lock;
  result = acquireHashLockFromZone(dataVIO->hashZone, &dataVIO->chunkName,
                                   NULL, &lock);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (isHashCollision(lock, dataVIO)) {
    // Hash collisions are extremely unlikely, but the bogus dedupe would be a
    // data corruption. Bypass dedupe entirely by leaving hashLock unset.
    // XXX clear hashZone too?
    return VDO_SUCCESS;
  }

  setHashLock(dataVIO, lock);
  return VDO_SUCCESS;
}

/**********************************************************************/
void releaseHashLock(DataVIO *dataVIO)
{
  HashLock *lock = dataVIO->hashLock;
  if (lock == NULL) {
    return;
  }

  setHashLock(dataVIO, NULL);

  if (lock->referenceCount > 0) {
    // The lock is still in use by other DataVIOs.
    return;
  }

  setHashLockState(lock, HASH_LOCK_DESTROYING);
  returnHashLockToZone(dataVIO->hashZone, &lock);
}

/**
 * Transfer a DataVIO's downgraded allocation PBN lock to the DataVIO's hash
 * lock, converting it to a duplicate PBN lock.
 *
 * @param dataVIO  The DataVIO holding the allocation lock to transfer
 **/
static void transferAllocationLock(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(dataVIO->newMapped.pbn == getDataVIOAllocation(dataVIO),
                  "transferred lock must be for the block written");

  AllocatingVIO *allocatingVIO  = dataVIOAsAllocatingVIO(dataVIO);
  PBNLock       *pbnLock        = allocatingVIO->allocationLock;
  allocatingVIO->allocationLock = NULL;
  allocatingVIO->allocation     = ZERO_BLOCK;

  ASSERT_LOG_ONLY(isPBNReadLock(pbnLock),
                  "must have downgraded the allocation lock before transfer");

  HashLock *hashLock  = dataVIO->hashLock;
  hashLock->duplicate = dataVIO->newMapped;
  dataVIO->duplicate  = dataVIO->newMapped;

  // Since the lock is being transferred, the holder count doesn't change (and
  // isn't even safe to examine on this thread).
  hashLock->duplicateLock = pbnLock;
}

/**********************************************************************/
void shareCompressedWriteLock(DataVIO *dataVIO, PBNLock *pbnLock)
{
  ASSERT_LOG_ONLY(getDuplicateLock(dataVIO) == NULL,
                  "a duplicate PBN lock should not exist when writing");
  ASSERT_LOG_ONLY(isCompressed(dataVIO->newMapped.state),
                  "lock transfer must be for a compressed write");
  assertInNewMappedZone(dataVIO);

  // First sharer downgrades the lock.
  if (!isPBNReadLock(pbnLock)) {
    downgradePBNWriteLock(pbnLock);
  }

  // Get a share of the PBN lock, ensuring it cannot be released until
  // after this DataVIO has had a chance to journal a reference.
  dataVIO->duplicate = dataVIO->newMapped;
  dataVIO->hashLock->duplicate = dataVIO->newMapped;
  setDuplicateLock(dataVIO->hashLock, pbnLock);

  // Claim a reference for this DataVIO, which is necessary since another
  // HashLock might start deduplicating against it before our incRef.
  bool claimed = claimPBNLockIncrement(pbnLock);
  ASSERT_LOG_ONLY(claimed, "impossible to fail to claim an initial increment");
}
