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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dedupeIndex.c#3 $
 */

#include "dedupeIndex.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"
#include "numeric.h"
#include "stringUtils.h"
#include "uds-block.h"

/*****************************************************************************/

typedef struct udsAttribute {
  struct attribute attr;
  const char *(*showString)(DedupeIndex *);
} UDSAttribute;

/*****************************************************************************/

enum { UDS_Q_ACTION };

/*****************************************************************************/

// These are the values in the atomic dedupeContext.requestState field
enum {
  // The UdsRequest object is not in use.
  UR_IDLE = 0,
  // The UdsRequest object is in use, and VDO is waiting for the result.
  UR_BUSY = 1,
  // The UdsRequest object is in use, but has timed out.
  UR_TIMED_OUT = 2,
};

/*****************************************************************************/

typedef enum {
  // The UDS index is closed
  IS_CLOSED = 0,
  // The UDS index is half-open.
  // There is a UdsIndexSession, but no UdsBlockContext.
  IS_INDEXSESSION = 1,
  // The UDS index is open.  There is a UdsIndexSession and a UdsBlockContext.
  IS_OPENED = 2,
} IndexState;

/*****************************************************************************/

typedef struct udsIndex {
  DedupeIndex        common;
  struct kobject     dedupeObject;
  KvdoWorkItem       workItem;
  RegisteredThread   allocatingThread;
  char              *indexName;
  UdsConfiguration   configuration;
  UdsBlockContext    blockContext;
  UdsIndexSession    indexSession;
  atomic_t           active;
  // This spinlock protects the state fields and the starting of dedupe
  // requests.
  spinlock_t         stateLock;
  KvdoWorkQueue     *udsQueue;    // protected by stateLock
  unsigned int       maximum;     // protected by stateLock
  IndexState         indexState;  // protected by stateLock
  IndexState         indexTarget; // protected by stateLock
  bool               changing;    // protected by stateLock
  bool               createFlag;  // protected by stateLock
  bool               dedupeFlag;  // protected by stateLock
  bool               deduping;    // protected by stateLock
  bool               errorFlag;   // protected by stateLock
  // This spinlock protects the pending list, the pending flag in each KVIO,
  // and the timeout list.
  spinlock_t         pendingLock;
  struct list_head   pendingHead;  // protected by pendingLock
  struct timer_list  pendingTimer; // protected by pendingLock
  bool               startedTimer; // protected by pendingLock
} UDSIndex;

/*****************************************************************************/

// Version 1:  user space albireo index (limited to 32 bytes)
// Version 2:  kernel space albireo index (limited to 16 bytes)
enum {
  UDS_ADVICE_VERSION = 2,
  // version byte + state byte + 64-bit little-endian PBN
  UDS_ADVICE_SIZE    = 1 + 1 + sizeof(uint64_t),
};

/*****************************************************************************/

  // We want to ensure that there is only one copy of the following constants.
static const char *CLOSED  = "closed";
static const char *CLOSING = "closing";
static const char *ERROR   = "error";
static const char *OFFLINE = "offline";
static const char *ONLINE  = "online";
static const char *OPENING = "opening";
static const char *UNKNOWN = "unknown";

/*****************************************************************************/

// These times are in milliseconds, and these are the default values.
unsigned int albireoTimeoutInterval  = 5000;
unsigned int minAlbireoTimerInterval = 100;

// These times are in jiffies
static Jiffies albireoTimeoutJiffies = 0;
static Jiffies minAlbireoTimerJiffies = 0;

/*****************************************************************************/
static const char *indexStateToString(UDSIndex *index, IndexState state)
{
  switch (state) {
  case IS_CLOSED:
    // Closed.  The errorFlag tells if it is because of an error.
    return index->errorFlag ? ERROR : CLOSED;
  case IS_INDEXSESSION:
    // The indexTarget tells if we are opening or closing the index.
    return index->indexTarget == IS_OPENED ? OPENING : CLOSING;
  case IS_OPENED:
    // Opened.  The dedupeFlag tells if we are online or offline.
    return index->dedupeFlag ? ONLINE : OFFLINE;
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
static void encodeUDSAdvice(UdsRequest *request, DataLocation advice)
{
  size_t offset = 0;
  UdsChunkData *encoding = &request->newMetadata;
  encoding->data[offset++] = UDS_ADVICE_VERSION;
  encoding->data[offset++] = advice.state;
  encodeUInt64LE(encoding->data, &offset, advice.pbn);
  BUG_ON(offset != UDS_ADVICE_SIZE);
}

/**
 * Decode VDO duplicate advice from the oldMetadata field of a UDS request.
 *
 * @param request  The UDS request containing the encoding
 * @param advice   The DataLocation to receive the decoded advice
 *
 * @return <code>true</code> if valid advice was found and decoded
 **/
static bool decodeUDSAdvice(const UdsRequest *request, DataLocation *advice)
{
  if ((request->status != UDS_SUCCESS) || !request->found) {
    return false;
  }

  size_t offset = 0;
  const UdsChunkData *encoding = &request->oldMetadata;
  byte version = encoding->data[offset++];
  if (version != UDS_ADVICE_VERSION) {
    logError("invalid UDS advice version code %u", version);
    return false;
  }

  advice->state = encoding->data[offset++];
  decodeUInt64LE(encoding->data, &offset, &advice->pbn);
  BUG_ON(offset != UDS_ADVICE_SIZE);
  return true;
}

/**
 * Calculate the actual end of a timer, taking into account the absolute start
 * time and the present time.
 *
 * @param startJiffies  The absolute start time, in jiffies
 *
 * @return the absolute end time for the timer, in jiffies
 **/
static Jiffies getAlbireoTimeout(Jiffies startJiffies)
{
  return maxULong(startJiffies + albireoTimeoutJiffies,
                  jiffies + minAlbireoTimerJiffies);
}

/*****************************************************************************/
void setAlbireoTimeoutInterval(unsigned int value)
{
  // Arbitrary maximum value is two minutes
  if (value > 120000) {
    value = 120000;
  }
  // Arbitrary minimum value is 2 jiffies
  Jiffies albJiffies = msecs_to_jiffies(value);
  if (albJiffies < 2) {
    albJiffies = 2;
    value      = jiffies_to_msecs(albJiffies);
  }
  albireoTimeoutInterval = value;
  albireoTimeoutJiffies  = albJiffies;
}

/*****************************************************************************/
void setMinAlbireoTimerInterval(unsigned int value)
{
  // Arbitrary maximum value is one second
  if (value > 1000) {
    value = 1000;
  }

  // Arbitrary minimum value is 2 jiffies
  Jiffies minJiffies = msecs_to_jiffies(value);
  if (minJiffies < 2) {
    minJiffies = 2;
    value = jiffies_to_msecs(minJiffies);
  }

  minAlbireoTimerInterval = value;
  minAlbireoTimerJiffies  = minJiffies;
}

/*****************************************************************************/
static void finishIndexOperation(UdsRequest *udsRequest)
{
  DataKVIO *dataKVIO = container_of(udsRequest, DataKVIO,
                                    dedupeContext.udsRequest);
  DedupeContext *dedupeContext = &dataKVIO->dedupeContext;
  if (compareAndSwap32(&dedupeContext->requestState, UR_BUSY, UR_IDLE)) {
    KVIO *kvio = dataKVIOAsKVIO(dataKVIO);
    UDSIndex *index = container_of(kvio->layer->dedupeIndex, UDSIndex, common);

    spin_lock_bh(&index->pendingLock);
    if (dedupeContext->isPending) {
      list_del(&dedupeContext->pendingList);
      dedupeContext->isPending = false;
    }
    spin_unlock_bh(&index->pendingLock);

    dedupeContext->status = udsRequest->status;
    if ((udsRequest->type == UDS_POST) || (udsRequest->type == UDS_QUERY)) {
      DataLocation advice;
      if (decodeUDSAdvice(udsRequest, &advice)) {
        setDedupeAdvice(dedupeContext, &advice);
      } else {
        setDedupeAdvice(dedupeContext, NULL);
      }
    }
    invokeDedupeCallback(dataKVIO);
    atomic_dec(&index->active);
  } else {
    compareAndSwap32(&dedupeContext->requestState, UR_TIMED_OUT, UR_IDLE);
  }
}

/*****************************************************************************/
static void startExpirationTimer(UDSIndex *index, DataKVIO *dataKVIO)
{
  if (!index->startedTimer) {
    index->startedTimer = true;
    mod_timer(&index->pendingTimer,
              getAlbireoTimeout(dataKVIO->dedupeContext.submissionTime));
  }
}

/*****************************************************************************/
static void startIndexOperation(KvdoWorkItem *item)
{
  KVIO *kvio = workItemAsKVIO(item);
  DataKVIO *dataKVIO = kvioAsDataKVIO(kvio);
  UDSIndex *index = container_of(kvio->layer->dedupeIndex, UDSIndex, common);
  DedupeContext *dedupeContext = &dataKVIO->dedupeContext;

  spin_lock_bh(&index->pendingLock);
  list_add_tail(&dedupeContext->pendingList, &index->pendingHead);
  dedupeContext->isPending = true;
  startExpirationTimer(index, dataKVIO);
  spin_unlock_bh(&index->pendingLock);

  UdsRequest *udsRequest = &dedupeContext->udsRequest;
  int status = udsStartChunkOperation(udsRequest);
  if (status != UDS_SUCCESS) {
    udsRequest->status = status;
    finishIndexOperation(udsRequest);
  }
}

/*****************************************************************************/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
static void timeoutIndexOperations(struct timer_list *t)
#else
static void timeoutIndexOperations(unsigned long arg)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
  UDSIndex *index = from_timer(index, t, pendingTimer);
#else
  UDSIndex *index = (UDSIndex *) arg;
#endif
  LIST_HEAD(expiredHead);
  uint64_t timeoutJiffies = msecs_to_jiffies(albireoTimeoutInterval);
  unsigned long earliestSubmissionAllowed = jiffies - timeoutJiffies;
  spin_lock_bh(&index->pendingLock);
  index->startedTimer = false;
  while (!list_empty(&index->pendingHead)) {
    DataKVIO *dataKVIO = list_first_entry(&index->pendingHead, DataKVIO,
                                          dedupeContext.pendingList);
    DedupeContext *dedupeContext = &dataKVIO->dedupeContext;
    if (earliestSubmissionAllowed <= dedupeContext->submissionTime) {
      startExpirationTimer(index, dataKVIO);
      break;
    }
    list_del(&dedupeContext->pendingList);
    dedupeContext->isPending = false;
    list_add_tail(&dedupeContext->pendingList, &expiredHead);
  }
  spin_unlock_bh(&index->pendingLock);
  while (!list_empty(&expiredHead)) {
    DataKVIO *dataKVIO = list_first_entry(&expiredHead, DataKVIO,
                                          dedupeContext.pendingList);
    DedupeContext *dedupeContext = &dataKVIO->dedupeContext;
    list_del(&dedupeContext->pendingList);
    if (compareAndSwap32(&dedupeContext->requestState,
                         UR_BUSY, UR_TIMED_OUT)) {
      dedupeContext->status = ETIMEDOUT;
      invokeDedupeCallback(dataKVIO);
      atomic_dec(&index->active);
      kvdoReportDedupeTimeout(dataKVIOAsKVIO(dataKVIO)->layer, 1);
    }
  }
}

/*****************************************************************************/
static void enqueueIndexOperation(DataKVIO        *dataKVIO,
                                  UdsCallbackType  operation)
{
  KVIO *kvio = dataKVIOAsKVIO(dataKVIO);
  DedupeContext *dedupeContext = &dataKVIO->dedupeContext;
  UDSIndex *index = container_of(kvio->layer->dedupeIndex, UDSIndex, common);
  dedupeContext->status         = UDS_SUCCESS;
  dedupeContext->submissionTime = jiffies;
  if (compareAndSwap32(&dedupeContext->requestState, UR_IDLE, UR_BUSY)) {
    UdsRequest *udsRequest = &dataKVIO->dedupeContext.udsRequest;
    udsRequest->chunkName = *dedupeContext->chunkName;
    udsRequest->callback  = finishIndexOperation;
    udsRequest->context   = index->blockContext;
    udsRequest->type      = operation;
    udsRequest->update    = true;
    if ((operation == UDS_POST) || (operation == UDS_UPDATE)) {
      encodeUDSAdvice(udsRequest, getDedupeAdvice(dedupeContext));
    }

    setupWorkItem(&kvio->enqueueable.workItem, startIndexOperation, NULL,
                  UDS_Q_ACTION);

    spin_lock(&index->stateLock);
    if (index->deduping) {
      enqueueWorkQueue(index->udsQueue, &kvio->enqueueable.workItem);
      unsigned int active = atomic_inc_return(&index->active);
      if (active > index->maximum) {
        index->maximum = active;
      }
      kvio = NULL;
    } else {
      atomicStore32(&dedupeContext->requestState, UR_IDLE);
    }
    spin_unlock(&index->stateLock);
  } else {
    // A previous user of the KVIO had a dedupe timeout
    // and its request is still outstanding.
    atomic64_inc(&kvio->layer->dedupeContextBusy);
  }
  if (kvio != NULL) {
    invokeDedupeCallback(dataKVIO);
  }
}

/*****************************************************************************/
static void closeContext(UDSIndex *index)
{
  // ASSERTION: We enter in IS_OPENED state.  Change the index state so that
  // the it will be reported to the outside world as "closing".
  index->indexState = IS_INDEXSESSION;
  // Close the block context, while not holding the stateLock.
  spin_unlock(&index->stateLock);
  int result = udsCloseBlockContext(index->blockContext);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Error closing block context for %s",
                            index->indexName);
  }
  spin_lock(&index->stateLock);
  // ASSERTION: We leave in IS_INDEXSESSION state.
}

/*****************************************************************************/
static void closeSession(UDSIndex *index)
{
  // ASSERTION: We enter in IS_INDEXSESSION state.
  // Close the index session, while not holding the stateLock.
  spin_unlock(&index->stateLock);
  int result = udsCloseIndexSession(index->indexSession);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Error closing index %s",
                            index->indexName);
  }
  spin_lock(&index->stateLock);
  index->indexState = IS_CLOSED;
  index->errorFlag |= result != UDS_SUCCESS;
  // ASSERTION: We leave in IS_CLOSED state.
}

/*****************************************************************************/
static void openContext(UDSIndex *index)
{
  // ASSERTION: We enter in IS_INDEXSESSION state.
  // Open the block context, while not holding the stateLock.
  spin_unlock(&index->stateLock);
  int result = udsOpenBlockContext(index->indexSession, UDS_ADVICE_SIZE,
                                   &index->blockContext);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Error closing block context for %s",
                            index->indexName);
  }
  spin_lock(&index->stateLock);
  if (result == UDS_SUCCESS) {
    index->indexState = IS_OPENED;
    // ASSERTION: On success, we leave in IS_OPENED state.
  } else {
    index->indexTarget = IS_CLOSED;
    index->errorFlag = true;
    // ASSERTION: On failure, we leave in IS_INDEXSESSION state.
  }
}

/*****************************************************************************/
static void openSession(UDSIndex *index)
{
  // ASSERTION: We enter in IS_CLOSED state.
  bool createFlag = index->createFlag;
  index->createFlag = false;
  // Change the index state so that the it will be reported to the outside
  // world as "opening".
  index->indexState = IS_INDEXSESSION;
  index->errorFlag = false;
  // Open the index session, while not holding the stateLock
  spin_unlock(&index->stateLock);
  bool nextCreateFlag = false;
  int result = UDS_SUCCESS;
  if (createFlag) {
    result = udsCreateLocalIndex(index->indexName, index->configuration,
                                 &index->indexSession);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "Error creating index %s",
                              index->indexName);
    }
  } else {
    result = udsRebuildLocalIndex(index->indexName, &index->indexSession);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "Error opening index %s",
                              index->indexName);
    } else {
      UdsConfiguration configuration;
      result = udsGetIndexConfiguration(index->indexSession, &configuration);
      if (result != UDS_SUCCESS) {
        logErrorWithStringError(result, "Error reading configuration for %s",
                                index->indexName);
        int closeResult = udsCloseIndexSession(index->indexSession);
        if (closeResult != UDS_SUCCESS) {
          logErrorWithStringError(closeResult, "Error closing index %s",
                                  index->indexName);
        }
      } else {
        if (udsConfigurationGetNonce(index->configuration)
            != udsConfigurationGetNonce(configuration)) {
          logError("Index does not belong to this VDO device");
          // We have an index, but it was made for some other VDO device.  We
          // will close the index and then try to create a new index.
          nextCreateFlag = true;
        }
        udsFreeConfiguration(configuration);
      }
    }
  }
  spin_lock(&index->stateLock);
  if (nextCreateFlag) {
    index->createFlag = true;
  }
  if (!createFlag) {
    switch (result) {
    case UDS_CORRUPT_COMPONENT:
    case UDS_NO_INDEX:
      // Either there is no index, or there is no way we can recover the index.
      // We will be called again and try to create a new index.
      index->indexState = IS_CLOSED;
      index->createFlag = true;
      return;
    default:
      break;
    }
  }
  if (result != UDS_SUCCESS) {
    index->indexState  = IS_CLOSED;
    index->indexTarget = IS_CLOSED;
    index->errorFlag   = true;
    spin_unlock(&index->stateLock);
    logInfo("Setting UDS index target state to error");
    spin_lock(&index->stateLock);
  }
  // ASSERTION: On success, we leave in IS_INDEXSESSION state.
  // ASSERTION: On failure, we leave in IS_CLOSED state.
}

/*****************************************************************************/
static void changeDedupeState(KvdoWorkItem *item)
{
  UDSIndex *index = container_of(item, UDSIndex, workItem);
  spin_lock(&index->stateLock);
  // Loop until the index is in the target state and the create flag is clear.
  while ((index->indexState != index->indexTarget) || index->createFlag) {
    if (index->indexState == IS_OPENED) {
      // We need to close the block context.  Either we want to be closed, or
      // we need to close it in order to create a new index.
      closeContext(index);
    } else if (index->indexState == IS_CLOSED) {
      // We need to open the index session.
      openSession(index);
    } else if ((index->indexTarget == IS_CLOSED) || index->createFlag) {
      // We need to close the index session.  Either we want to be closed, or
      // we need to close it in order to create a new index.
      closeSession(index);
    } else {
      // We need to open the block context.
      openContext(index);
    }
  }
  index->changing = false;
  index->deduping = index->dedupeFlag && (index->indexState == IS_OPENED);
  spin_unlock(&index->stateLock);
}

/*****************************************************************************/
static void setTargetState(UDSIndex   *index,
                           IndexState  target,
                           bool        changeDedupe,
                           bool        dedupe,
                           bool        setCreate)
{
  spin_lock(&index->stateLock);
  const char *oldState = indexStateToString(index, index->indexTarget);
  if (changeDedupe) {
    index->dedupeFlag = dedupe;
  }
  if (setCreate) {
    index->createFlag = true;
  }
  if (index->changing) {
    // A change is already in progress, just change the target state
    index->indexTarget = target;
  } else if ((target != index->indexTarget) || setCreate) {
    // Must start a state change by enqueuing a work item that calls
    // changeDedupeState.
    index->indexTarget = target;
    index->changing = true;
    index->deduping = false;
    setupWorkItem(&index->workItem, changeDedupeState, NULL, UDS_Q_ACTION);
    enqueueWorkQueue(index->udsQueue, &index->workItem);
  } else {
    // Online vs. offline changes happen immediately
    index->deduping = index->dedupeFlag && (index->indexState == IS_OPENED);
  }
  const char *newState = indexStateToString(index, index->indexTarget);
  spin_unlock(&index->stateLock);
  if (oldState != newState) {
    logInfo("Setting UDS index target state to %s", newState);
  }
}

/*****************************************************************************/

/*****************************************************************************/
static void dumpUDSIndex(DedupeIndex *dedupeIndex, bool showQueue)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  spin_lock(&index->stateLock);
  const char *state = indexStateToString(index, index->indexState);
  const char *target = (index->changing
                        ? indexStateToString(index, index->indexTarget)
                        : NULL);
  spin_unlock(&index->stateLock);
  logInfo("UDS index: state: %s", state);
  if (target != NULL) {
    logInfo("UDS index: changing to state: %s", target);
  }
  if (showQueue) {
    dumpWorkQueue(index->udsQueue);
  }
}

/*****************************************************************************/
static void finishUDSIndex(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  setTargetState(index, IS_CLOSED, false, false, false);
  udsFreeConfiguration(index->configuration);
  finishWorkQueue(index->udsQueue);
}

/*****************************************************************************/
static void freeUDSIndex(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  freeWorkQueue(&index->udsQueue);
  spin_lock_bh(&index->pendingLock);
  if (index->startedTimer) {
    del_timer_sync(&index->pendingTimer);
  }
  spin_unlock_bh(&index->pendingLock);
  kobject_put(&index->dedupeObject);
}

/*****************************************************************************/
static const char *getUDSStateName(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  spin_lock(&index->stateLock);
  const char *state = indexStateToString(index, index->indexState);
  spin_unlock(&index->stateLock);
  return state;
}

/*****************************************************************************/
static void getUDSStatistics(DedupeIndex *dedupeIndex, IndexStatistics *stats)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  spin_lock(&index->stateLock);
  IndexState      indexState   = index->indexState;
  stats->maxDedupeQueries      = index->maximum;
  spin_unlock(&index->stateLock);
  stats->currDedupeQueries     = atomic_read(&index->active);
  if (indexState == IS_OPENED) {
    UdsIndexStats indexStats;
    int result = udsGetIndexStats(index->indexSession, &indexStats);
    if (result == UDS_SUCCESS) {
      stats->entriesIndexed = indexStats.entriesIndexed;
    } else {
      logErrorWithStringError(result, "Error reading index stats");
    }
    UdsContextStats contextStats;
    result = udsGetBlockContextStats(index->blockContext, &contextStats);
    if (result == UDS_SUCCESS) {
      stats->postsFound      = contextStats.postsFound;
      stats->postsNotFound   = contextStats.postsNotFound;
      stats->queriesFound    = contextStats.queriesFound;
      stats->queriesNotFound = contextStats.queriesNotFound;
      stats->updatesFound    = contextStats.updatesFound;
      stats->updatesNotFound = contextStats.updatesNotFound;
    } else {
      logErrorWithStringError(result, "Error reading context stats");
    }
  }
}


/*****************************************************************************/
static int processMessage(DedupeIndex *dedupeIndex, const char *name)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  if (strcasecmp(name, "index-create") == 0) {
    setTargetState(index, IS_OPENED, false, false, true);
    return 0;
  } else if (strcasecmp(name, "index-disable") == 0) {
    setTargetState(index, IS_OPENED, true, false, false);
    return 0;
  } else if (strcasecmp(name, "index-enable") == 0) {
    setTargetState(index, IS_OPENED, true, true, false);
    return 0;
  }
  return -EINVAL;
}

/*****************************************************************************/
static void udsPost(DataKVIO *dataKVIO)
{
  enqueueIndexOperation(dataKVIO, UDS_POST);
}

/*****************************************************************************/
static void udsQuery(DataKVIO *dataKVIO)
{
  enqueueIndexOperation(dataKVIO, UDS_QUERY);
}

/*****************************************************************************/
static void startUDSIndex(DedupeIndex *dedupeIndex, bool createFlag)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  setTargetState(index, IS_OPENED, true, true, createFlag);
}

/*****************************************************************************/
static void stopUDSIndex(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  setTargetState(index, IS_CLOSED, false, false, false);
}

/*****************************************************************************/
static void udsUpdate(DataKVIO *dataKVIO)
{
  enqueueIndexOperation(dataKVIO, UDS_UPDATE);
}

/*****************************************************************************/
static void dedupeKobjRelease(struct kobject *kobj)
{
  UDSIndex *index = container_of(kobj, UDSIndex, dedupeObject);
  FREE(index->indexName);
  FREE(index);
}

/*****************************************************************************/
static ssize_t dedupeStatusShow(struct kobject   *kobj,
                                struct attribute *attr,
                                char             *buf)
{
  UDSAttribute *ua = container_of(attr, UDSAttribute, attr);
  UDSIndex *index = container_of(kobj, UDSIndex, dedupeObject);
  if (ua->showString != NULL) {
    return sprintf(buf, "%s\n", ua->showString(&index->common));
  } else {
    return -EINVAL;
  }
}

/*****************************************************************************/
static ssize_t dedupeStatusStore(struct kobject   *kobj,
                                 struct attribute *attr,
                                 const char       *buf,
                                 size_t            length)
{
  return -EINVAL;
}

/*****************************************************************************/

static struct sysfs_ops dedupeSysfsOps = {
  .show  = dedupeStatusShow,
  .store = dedupeStatusStore,
};

static UDSAttribute dedupeStatusAttribute = {
  .attr = {.name = "status", .mode = 0444, },
  .showString = getUDSStateName,
};

static struct attribute *dedupeAttributes[] = {
  &dedupeStatusAttribute.attr,
  NULL,
};

static struct kobj_type dedupeKobjType = {
  .release       = dedupeKobjRelease,
  .sysfs_ops     = &dedupeSysfsOps,
  .default_attrs = dedupeAttributes,
};

/*****************************************************************************/
static void startUDSQueue(void *ptr)
{
  /*
   * Allow the UDS dedupe worker thread to do memory allocations.  It will
   * only do allocations during the UDS calls that open or close an index,
   * but those allocations can safely sleep while reserving a large amount
   * of memory.  We could use an allocationsAllowed boolean (like the base
   * threads do), but it would be an unnecessary embellishment.
   */
  UDSIndex *index = ptr;
  registerAllocatingThread(&index->allocatingThread, NULL);
}

/*****************************************************************************/
static void finishUDSQueue(void *ptr)
{
  unregisterAllocatingThread();
}

/*****************************************************************************/
int makeDedupeIndex(DedupeIndex **indexPtr, KernelLayer *layer)
{
  setAlbireoTimeoutInterval(albireoTimeoutInterval);
  setMinAlbireoTimerInterval(minAlbireoTimerInterval);

  UDSIndex *index;
  int result = ALLOCATE(1, UDSIndex, "UDS index data", &index);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = allocSprintf("index name", &index->indexName,
                        "dev=%s offset=4096 size=%llu",
                        layer->deviceConfig->parentDeviceName,
                        getIndexRegionSize(layer->geometry) * VDO_BLOCK_SIZE);
  if (result != UDS_SUCCESS) {
    logError("Creating index name failed (%d)", result);
    FREE(index);
    return result;
  }

  result = indexConfigToUdsConfiguration(&layer->geometry.indexConfig,
                                         &index->configuration);
  if (result != VDO_SUCCESS) {
    FREE(index->indexName);
    FREE(index);
    return result;
  }
  udsConfigurationSetNonce(index->configuration,
                           (UdsNonce) layer->geometry.nonce);

  static const KvdoWorkQueueType udsQueueType = {
    .start        = startUDSQueue,
    .finish       = finishUDSQueue,
    .actionTable  = {
      { .name = "uds_action", .code = UDS_Q_ACTION, .priority = 0 },
    },
  };
  result = makeWorkQueue(layer->threadNamePrefix, "dedupeQ",
                         &layer->wqDirectory, layer, index, &udsQueueType, 1,
                         &index->udsQueue);
  if (result != VDO_SUCCESS) {
    logError("UDS index queue initialization failed (%d)", result);
    udsFreeConfiguration(index->configuration);
    FREE(index->indexName);
    FREE(index);
    return result;
  }

  kobject_init(&index->dedupeObject, &dedupeKobjType);
  result = kobject_add(&index->dedupeObject, &layer->kobj, "dedupe");
  if (result != VDO_SUCCESS) {
    freeWorkQueue(&index->udsQueue);
    udsFreeConfiguration(index->configuration);
    FREE(index->indexName);
    FREE(index);
    return result;
  }

  index->common.dump                      = dumpUDSIndex;
  index->common.free                      = freeUDSIndex;
  index->common.getDedupeStateName        = getUDSStateName;
  index->common.getStatistics             = getUDSStatistics;
  index->common.message                   = processMessage;
  index->common.post                      = udsPost;
  index->common.query                     = udsQuery;
  index->common.start                     = startUDSIndex;
  index->common.stop                      = stopUDSIndex;
  index->common.finish                    = finishUDSIndex;
  index->common.update                    = udsUpdate;

  INIT_LIST_HEAD(&index->pendingHead);
  spin_lock_init(&index->pendingLock);
  spin_lock_init(&index->stateLock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
  timer_setup(&index->pendingTimer, timeoutIndexOperations, 0);
#else
  setup_timer(&index->pendingTimer, timeoutIndexOperations,
              (unsigned long) index);
#endif

  *indexPtr = &index->common;
  return VDO_SUCCESS;
}
