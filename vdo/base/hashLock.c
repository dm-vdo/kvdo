/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/hashLock.c#1 $
 */

/**
 * XXX VDOSTORY-190 Need more overview documentation of this module.
 *
 * The function names in this module follow a convention referencing the
 * states and transitions in the state machine diagram for VDOSTORY-190. [XXX
 * link or repository path to it?] For example, for the LOCKING state, there
 * are startLocking() and finishLocking() functions. startLocking() is invoked
 * by the finish function of the state (or states) that transition to LOCKING.
 * It performs the actual lock state change and must be invoked on the hash
 * zone thread. finishLocking() is called by (or continued via callback from)
 * the code actually obtaining the lock. It does any bookkeeping or
 * decision-making required and invokes the appropriate start function of the
 * state being transitioned to after LOCKING.
 **/

#include "hashLock.h"
#include "hashLockInternals.h"

#include "permassert.h"

#include "compressionState.h"
#include "constants.h"
#include "dataVIO.h"
#include "hashZone.h"
#include "packer.h"
#include "ringNode.h"
#include "types.h"
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

/**********************************************************************/
PBNLock *getDuplicateLock(DataVIO *dataVIO)
{
  if (dataVIO->duplicateLock != NULL) {
    return dataVIO->duplicateLock;
  }
  if (dataVIO->hashLock == NULL) {
    return NULL;
  }
  // XXX VDOSTORY-190: uncomment when HashLock has a duplicateLock field.
  return NULL;   // XXX dataVIO->hashLock->duplicateLock;
}

/**********************************************************************/
const char *getHashLockStateName(HashLockState state)
{
  // Catch if a state has been added without updating the name array.
  STATIC_ASSERT((HASH_LOCK_DESTROYING + 1) == COUNT_OF(LOCK_STATE_NAMES));
  return (state < COUNT_OF(LOCK_STATE_NAMES)) ? LOCK_STATE_NAMES[state] : NULL;
}

/**
 * Assert the current state of the hash lock.
 *
 * @param lock           The lock to update
 * @param expectedState  The expected current state
 * @param where          The name of the function making the assertion
 **/
static void assertHashLockState(HashLock      *lock,
                                HashLockState  expectedState,
                                const char    *where)
{
  ASSERT_LOG_ONLY(lock->state == expectedState,
                  "hash lock should be in state %s in %s",
                  getHashLockStateName(expectedState), where);
}

/**
 * Set the current state of a hash lock.
 *
 * @param lock      The lock to update
 * @param newState  The new state
 **/
static void setHashLockState(HashLock *lock, HashLockState newState)
{
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

  /*
   * XXX VDOSTORY-190 Releasing the hash lock before the allocation lock could
   * result in losing dedupe if a new write of the same data comes in, is
   * hashed, gets a hash lock, receives advice, and finds the write lock
   * before the agent cleans up and releases its write lock. Unlikely, but
   * possible. Doing it the other way around is also not ideal, since it means
   * the hash lock could be discovered while the write lock is no longer held,
   * which would require that we detect that had happened and clear the
   * verified flag. It's racy, which would probably mean not setting the flag
   * at all. That might be the best thing, but it would trigger a re-verify
   * that admittedly would happen anyway with slightly different timing.
   */

  // Complete the DataVIO and start the clean-up path in vioWrite to release
  // any locks it still holds.
  finishDataVIO(dataVIO, VDO_SUCCESS);
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
    // XXX VDOSTORY-190 Need proper handling here for this corrupt state,
    // but for now just toss it back to vioWrite to handle.
    continueDataVIOIn(dataVIO, result, verifyAdvice);
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
 * WaiterCallback function that copies a duplicate location to the duplicate
 * field of the DataVIO waiter, then re-queues the DataVIO to resume on the
 * write path with the callback set before enterHashLock().
 *
 * @param waiter   The DataVIO's waiter link
 * @param context  A pointer to the ZonedPBN of the duplicate block
 **/
static void requeueAdviceWaiter(Waiter *waiter, void *context)
{
  DataVIO  *dataVIO    = waiterAsDataVIO(waiter);
  ZonedPBN *duplicate  = context;
  setDuplicateLocation(dataVIO, *duplicate);

  VDOCompletion *completion = dataVIOAsCompletion(dataVIO);
  completion->requeue       = true;
  completion->callback      = verifyAdvice;
  invokeCallback(completion);
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
  // XXX This later may need to allow for more entry states, or the callers
  // will have to make the transition.
  setHashLockState(lock, HASH_LOCK_BYPASSING);
  setAgent(lock, NULL);

  if (!hasWaiters(&lock->waiters)) {
    if (agent == NULL) {
      return;
    }

    // No other DataVIOs are sharing the lock, so just resume the write path.
    continueDataVIOIn(agent, VDO_SUCCESS, verifyAdvice);
    return;
  }

  ASSERT_LOG_ONLY(agent != NULL, "can't have waiters but no agent");

  // Copy the advice out of the agent since we continue it first (to preserve
  // ordering), but it's not safe to reference after that.
  ZonedPBN duplicate
    = (agent->isDuplicate ? agent->duplicate : agent->newMapped);
  continueDataVIOIn(agent, VDO_SUCCESS, verifyAdvice);
  notifyAllWaiters(&lock->waiters, requeueAdviceWaiter, &duplicate);
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
  if (dataVIO == lock->agent) {
    /*
     * XXX VDOSTORY-190: For now, simply enter state BYPASSING so that
     * future VIOs do not get stuck in the hash lock. We can consider
     * more sophisticated handling later.
     */
    startBypassing(lock, dataVIO);
    return;
  }

  // XXX If this VIO is not the agent, it should exit without
  // altering the hash lock state.
  exitHashLock(dataVIO);
}

/**********************************************************************/
void continueHashLockOnError(DataVIO *dataVIO)
{
  // XXX We could simply use continueHashLock() and check for errors in that.
  abortHashLock(dataVIO->hashLock, dataVIO);
}

/**
 * Re-entry point for the lock agent after it has finished writing or
 * compressing its copy of the data block. The agent will never need to dedupe
 * against anything, so it's done with the lock, but the lock may not be
 * finished with it, as an Albireo update might still be needed.
 *
 * If there are other lock holders, the agent will hand the job to one of them
 * and exit, leaving the lock to deduplicate against the just-written block.
 * If there are no other lock holders, the agent either exits (and later tears
 * down the hash lock), or it remains the agent and updates Albireo.
 *
 * @param lock   The hash lock, which must be in state WRITING
 * @param agent  The DataVIO that wrote its data for the lock
 **/
static void finishWriting(HashLock *lock, DataVIO *agent)
{
  DataVIO *newAgent = waiterAsDataVIO(dequeueNextWaiter(&lock->waiters));
  if (newAgent != NULL) {
    setAgent(lock, newAgent);
    // Everyone else should dedupe against the data block or compressed block
    // slot the agent wrote.
    setDuplicateLocation(newAgent, agent->newMapped);
    // XXX startLocking() when it exists
    startBypassing(lock, newAgent);
  } else {
    // XXX VDOSTORY-190 have to decide whether an Albireo update is needed
    // here, but assume for now the write path has already updated Albireo.

    // BYPASSING is the state currently expected by releaseHashLock().
    startBypassing(lock, NULL);
  }

  // The old agent has finished the write path completely and has no more work
  // to do for the lock.
  exitHashLock(agent);
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
    ASSERT_LOG_ONLY(result == VDO_SUCCESS, "impossible enqueueDataVIO error");
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
   * finishWriting(). If it fails, it will call releaseHashLock() during
   * clean-up.
   */
  compressData(agent);
}

/**
 * Process the result of an Albireo query performed by the agent for the lock.
 * This continuation is registered in startQuerying().
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
    /*
     * QUERYING -> LOCKING transition: Valid advice was obtained from Albireo.
     * Use the QUERYING agent to start the hash lock on the unverified dedupe
     * path, verifying that the advice can be used.
     */
    // XXX VDOSTORY-190 Use startLocking(lock, agent) when it exists
    startBypassing(lock, agent);
  } else {
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
 * state, and using the agent to perform the Albireo query on behalf of the
 * lock.
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
 * Launch an Albiro query for a DataVIO independent of the lock, starting the
 * DataVIO on the old dedupe path. This will go away later in VDOSTORY-190.
 *
 * @param dataVIO  The DataVIO to process on the old dedupe path
 **/
static void launchOldDedupe(DataVIO *dataVIO)
{
  VDOCompletion *completion   = dataVIOAsCompletion(dataVIO);
  dataVIO->lastAsyncOperation = CHECK_FOR_DEDUPLICATION;
  completion->callback        = verifyAdvice;
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
  continueDataVIOIn(dataVIO, result, verifyAdvice);
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
  case HASH_LOCK_LOCKING:
  case HASH_LOCK_VERIFYING:
    // The lock is busy, and can't be shared yet.
    waitOnHashLock(lock, dataVIO);
    break;

  case HASH_LOCK_BYPASSING:
    // XXX Lots of VDOSTORY-190 stuff still to come, dispatching on the lock
    // state. For now, just proceed independently on the old dedupe path.
    launchOldDedupe(dataVIO);
    break;

  case HASH_LOCK_DEDUPING:
  case HASH_LOCK_UNLOCKING:
  case HASH_LOCK_UPDATING:
    // XXX These states are not yet implemented in VDOSTORY-190 and don't
    // know what do to.
    reportBogusLockState(lock, dataVIO);
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
  case HASH_LOCK_DEDUPING:
  case HASH_LOCK_UNLOCKING:
  case HASH_LOCK_DESTROYING:
    // A lock in this state should never be re-entered.
    reportBogusLockState(lock, dataVIO);
    break;

  default:
    reportBogusLockState(lock, dataVIO);
  }
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
  return !layer->compareDataVIOs(lockHolder, candidate);
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
  result = ASSERT(dataVIO->duplicateLock == NULL,
                  "must not hold a read lock when getting a hash lock");
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
                                   &lock);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (isHashCollision(lock, dataVIO)) {
    // Hash collisions are extremely unlikely, but the bogus dedupe would be a
    // data corruption. Bypass dedupe entirely by leaving hashLock unset.
    // XXX clear hashZone too?
    return VDO_SUCCESS;
  }

  // Keep all DataVIOs sharing the lock on a ring since they can complete in
  // any order and we'll always need a pointer to one to compare data.
  pushRingNode(&lock->duplicateRing, &dataVIO->hashLockNode);
  lock->referenceCount += 1;

  // XXX Not needed for VDOSTORY-190, but useful for checking whether a test
  // is getting concurrent dedupe, and how much.
  if (lock->maxReferences < lock->referenceCount) {
    lock->maxReferences = lock->referenceCount;
  }

  dataVIO->hashLock = lock;
  return VDO_SUCCESS;
}

/**********************************************************************/
void releaseHashLock(DataVIO *dataVIO)
{
  HashLock *lock = dataVIO->hashLock;
  if (lock == NULL) {
    return;
  }

  if (lock->state != HASH_LOCK_BYPASSING) {
    // This is what a write error currently looks like.
    ASSERT_LOG_ONLY(dataVIOAsCompletion(dataVIO)->result != VDO_SUCCESS,
                    "only DataVIO errors should leave locks not in BYPASS");
    ASSERT_LOG_ONLY(lock->state == HASH_LOCK_WRITING,
                    "currently must only see errors on lock agent writes");
    ASSERT_LOG_ONLY(lock->agent == dataVIO,
                    "currently must only see errors on the lock agent");

    DataVIO *newAgent = dequeueLockWaiter(lock);
    setAgent(lock, newAgent);
    startBypassing(lock, newAgent);
  }

  dataVIO->hashLock = NULL;

  ASSERT_LOG_ONLY(dataVIO->hashZone != NULL,
                  "must have a hash zone when halding a hash lock");
  ASSERT_LOG_ONLY(!isRingEmpty(&dataVIO->hashLockNode),
                  "must be on a hash lock ring when holding a hash lock");
  ASSERT_LOG_ONLY(lock->referenceCount > 0,
                  "hash lock being released must be referenced");

  unspliceRingNode(&dataVIO->hashLockNode);
  if (--lock->referenceCount > 0) {
    // The lock is still in use by other DataVIOs.
    return;
  }

  assertHashLockState(lock, HASH_LOCK_BYPASSING, __func__);

  /*
   * We held the last reference to this lock, so it should release any
   * resources it holds (such as a PBN read lock), then give the lock back to
   * its zone.
   */

  setHashLockState(lock, HASH_LOCK_DESTROYING);
  // XXX VDOSTORY-190 work goes here.

  returnHashLockToZone(dataVIO->hashZone, &lock);
}
