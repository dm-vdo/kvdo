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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dedupeIndex.c#18 $
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
  const char *(*showString)(struct dedupe_index *);
} UDSAttribute;

/*****************************************************************************/

typedef struct dedupeSuspend {
  struct kvdo_work_item  workItem;
  struct completion      completion;
  struct dedupe_index   *index;
  bool                   saveFlag;
} DedupeSuspend;

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
  IS_CLOSED   = 0,
  // The UdsIndexSession is opening or closing
  IS_CHANGING = 1,
  // The UDS index is open.
  IS_OPENED   = 2,
} IndexState;

enum {
  DEDUPE_TIMEOUT_REPORT_INTERVAL = 1000,
};

// Data managing the reporting of UDS timeouts
typedef struct periodicEventReporter {
  uint64_t               lastReportedValue;
  const char            *format;
  atomic64_t             value;
  Jiffies                reportingInterval; // jiffies
  /*
   * Just an approximation.  If nonzero, then either the work item has
   * been queued to run, or some other thread currently has
   * responsibility for enqueueing it, or the reporter function is
   * running but hasn't looked at the current value yet.
   *
   * If this is set, don't set the timer again, because we don't want
   * the work item queued twice.  Use an atomic xchg or cmpxchg to
   * test-and-set it, and an atomic store to clear it.
   */
  atomic_t               workItemQueued;
  struct kvdo_work_item  workItem;
  KernelLayer           *layer;
} PeriodicEventReporter;

/*****************************************************************************/

struct dedupe_index {
  struct kobject           dedupeObject;
  RegisteredThread         allocatingThread;
  char                    *indexName;
  UdsConfiguration         configuration;
  UdsIndexSession          indexSession;
  atomic_t                 active;
  // for reporting UDS timeouts
  PeriodicEventReporter    timeoutReporter;
  // This spinlock protects the state fields and the starting of dedupe
  // requests.
  spinlock_t               stateLock;
  struct kvdo_work_item    workItem;    // protected by stateLock
  struct kvdo_work_queue  *udsQueue;    // protected by stateLock
  unsigned int             maximum;     // protected by stateLock
  IndexState               indexState;  // protected by stateLock
  IndexState               indexTarget; // protected by stateLock
  bool                     changing;    // protected by stateLock
  bool                     createFlag;  // protected by stateLock
  bool                     dedupeFlag;  // protected by stateLock
  bool                     deduping;    // protected by stateLock
  bool                     errorFlag;   // protected by stateLock
  // This spinlock protects the pending list, the pending flag in each kvio,
  // and the timeout list.
  spinlock_t               pendingLock;
  struct list_head         pendingHead;  // protected by pendingLock
  struct timer_list        pendingTimer; // protected by pendingLock
  bool                     startedTimer; // protected by pendingLock
};

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
static const char *indexStateToString(struct dedupe_index *index,
                                      IndexState state)
{
  switch (state) {
  case IS_CLOSED:
    // Closed.  The errorFlag tells if it is because of an error.
    return index->errorFlag ? ERROR : CLOSED;
  case IS_CHANGING:
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
  struct data_kvio *dataKVIO = container_of(udsRequest,
                                            struct data_kvio,
                                            dedupeContext.udsRequest);
  struct dedupe_context *dedupeContext = &dataKVIO->dedupeContext;
  if (compareAndSwap32(&dedupeContext->requestState, UR_BUSY, UR_IDLE)) {
    struct kvio *kvio = data_kvio_as_kvio(dataKVIO);
    struct dedupe_index *index = kvio->layer->dedupeIndex;

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
        set_dedupe_advice(dedupeContext, &advice);
      } else {
        set_dedupe_advice(dedupeContext, NULL);
      }
    }
    invokeDedupeCallback(dataKVIO);
    atomic_dec(&index->active);
  } else {
    compareAndSwap32(&dedupeContext->requestState, UR_TIMED_OUT, UR_IDLE);
  }
}

/*****************************************************************************/
static void suspendIndex(struct kvdo_work_item *item)
{
  DedupeSuspend *dedupeSuspend = container_of(item, DedupeSuspend, workItem);
  struct dedupe_index *index = dedupeSuspend->index;
  spin_lock(&index->stateLock);
  IndexState indexState = index->indexState;
  spin_unlock(&index->stateLock);
  if (indexState == IS_OPENED) {
    int result = UDS_SUCCESS;
    if (dedupeSuspend->saveFlag) {
      result = udsSaveIndex(index->indexSession);
    } else {      
      result = udsFlushIndexSession(index->indexSession);
    }
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "Error suspending dedupe index");
    }
  }
  complete(&dedupeSuspend->completion);
}

/*****************************************************************************/
void suspendDedupeIndex(struct dedupe_index *index, bool saveFlag)
{
  DedupeSuspend dedupeSuspend = {
    .index    = index,
    .saveFlag = saveFlag,
  };
  init_completion(&dedupeSuspend.completion);
  setup_work_item(&dedupeSuspend.workItem, suspendIndex, NULL, UDS_Q_ACTION);
  enqueue_work_queue(index->udsQueue, &dedupeSuspend.workItem);
  wait_for_completion(&dedupeSuspend.completion);
}

/*****************************************************************************/
static void startExpirationTimer(struct dedupe_index *index,
                                 struct data_kvio    *dataKVIO)
{
  if (!index->startedTimer) {
    index->startedTimer = true;
    mod_timer(&index->pendingTimer,
              getAlbireoTimeout(dataKVIO->dedupeContext.submissionTime));
  }
}

/*****************************************************************************/
static void startIndexOperation(struct kvdo_work_item *item)
{
  struct kvio *kvio = work_item_as_kvio(item);
  struct data_kvio *dataKVIO = kvio_as_data_kvio(kvio);
  struct dedupe_index *index = kvio->layer->dedupeIndex;
  struct dedupe_context *dedupeContext = &dataKVIO->dedupeContext;

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

/**********************************************************************/
uint64_t getDedupeTimeoutCount(struct dedupe_index *index)
{
  return atomic64_read(&index->timeoutReporter.value);
}

/**********************************************************************/
static void reportEvents(PeriodicEventReporter *reporter)
{
  atomic_set(&reporter->workItemQueued, 0);
  uint64_t newValue = atomic64_read(&reporter->value);
  uint64_t difference = newValue - reporter->lastReportedValue;
  if (difference != 0) {
    logDebug(reporter->format, difference);
    reporter->lastReportedValue = newValue;
  }
}

/**********************************************************************/
static void reportEventsWork(struct kvdo_work_item *item)
{
  PeriodicEventReporter *reporter = container_of(item, PeriodicEventReporter,
                                                 workItem);
  reportEvents(reporter);
}

/**********************************************************************/
static void initPeriodicEventReporter(PeriodicEventReporter *reporter,
                                      const char            *format,
                                      unsigned long          reportingInterval,
                                      KernelLayer           *layer)
{
  setup_work_item(&reporter->workItem, reportEventsWork, NULL,
                  CPU_Q_ACTION_EVENT_REPORTER);
  reporter->format            = format;
  reporter->reportingInterval = msecs_to_jiffies(reportingInterval);
  reporter->layer             = layer;
}

/**
 * Record and eventually report that a dedupe request reached its expiration
 * time without getting an answer, so we timed it out.
 * 
 * This is called in a timer context, so it shouldn't do the reporting
 * directly.
 *
 * @param reporter       The periodic event reporter
 **/
static void reportDedupeTimeout(PeriodicEventReporter *reporter)
{
  atomic64_inc(&reporter->value);
  int oldWorkItemQueued = atomic_xchg(&reporter->workItemQueued, 1);
  if (oldWorkItemQueued == 0) {
    enqueue_work_queue_delayed(reporter->layer->cpuQueue,
                               &reporter->workItem,
                               jiffies + reporter->reportingInterval);
  }
}

/**********************************************************************/
static void stopPeriodicEventReporter(PeriodicEventReporter *reporter)
{
  reportEvents(reporter);
}

/*****************************************************************************/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
static void timeoutIndexOperations(struct timer_list *t)
#else
static void timeoutIndexOperations(unsigned long arg)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
  struct dedupe_index *index = from_timer(index, t, pendingTimer);
#else
  struct dedupe_index *index = (struct dedupe_index *) arg;
#endif
  LIST_HEAD(expiredHead);
  uint64_t timeoutJiffies = msecs_to_jiffies(albireoTimeoutInterval);
  unsigned long earliestSubmissionAllowed = jiffies - timeoutJiffies;
  spin_lock_bh(&index->pendingLock);
  index->startedTimer = false;
  while (!list_empty(&index->pendingHead)) {
    struct data_kvio *dataKVIO = list_first_entry(&index->pendingHead,
                                                  struct data_kvio,
                                                  dedupeContext.pendingList);
    struct dedupe_context *dedupeContext = &dataKVIO->dedupeContext;
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
    struct data_kvio *dataKVIO = list_first_entry(&expiredHead,
                                                  struct data_kvio,
                                                  dedupeContext.pendingList);
    struct dedupe_context *dedupeContext = &dataKVIO->dedupeContext;
    list_del(&dedupeContext->pendingList);
    if (compareAndSwap32(&dedupeContext->requestState,
                         UR_BUSY, UR_TIMED_OUT)) {
      dedupeContext->status = ETIMEDOUT;
      invokeDedupeCallback(dataKVIO);
      atomic_dec(&index->active);
      reportDedupeTimeout(&index->timeoutReporter);
    }
  }
}

/*****************************************************************************/
static void enqueueIndexOperation(struct data_kvio *dataKVIO,
                                  UdsCallbackType   operation)
{
  struct kvio *kvio = data_kvio_as_kvio(dataKVIO);
  struct dedupe_context *dedupeContext = &dataKVIO->dedupeContext;
  struct dedupe_index *index = kvio->layer->dedupeIndex;
  dedupeContext->status = UDS_SUCCESS;
  dedupeContext->submissionTime = jiffies;
  if (compareAndSwap32(&dedupeContext->requestState, UR_IDLE, UR_BUSY)) {
    UdsRequest *udsRequest = &dataKVIO->dedupeContext.udsRequest;
    udsRequest->chunkName = *dedupeContext->chunkName;
    udsRequest->callback  = finishIndexOperation;
    udsRequest->session   = index->indexSession;
    udsRequest->type      = operation;
    udsRequest->update    = true;
    if ((operation == UDS_POST) || (operation == UDS_UPDATE)) {
      encodeUDSAdvice(udsRequest, get_dedupe_advice(dedupeContext));
    }

    setup_work_item(&kvio->enqueueable.workItem, startIndexOperation, NULL,
                    UDS_Q_ACTION);

    spin_lock(&index->stateLock);
    if (index->deduping) {
      enqueue_work_queue(index->udsQueue, &kvio->enqueueable.workItem);
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
    // A previous user of the kvio had a dedupe timeout
    // and its request is still outstanding.
    atomic64_inc(&kvio->layer->dedupeContextBusy);
  }
  if (kvio != NULL) {
    invokeDedupeCallback(dataKVIO);
  }
}

/*****************************************************************************/
static void closeSession(struct dedupe_index *index)
{
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
static void openSession(struct dedupe_index *index)
{
  // ASSERTION: We enter in IS_CLOSED state.
  bool createFlag = index->createFlag;
  index->createFlag = false;
  // Change the index state so that the it will be reported to the outside
  // world as "opening".
  index->indexState = IS_CHANGING;
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
  if (result == UDS_SUCCESS) {
    index->indexState = IS_OPENED;
  } else {
    index->indexState  = IS_CLOSED;
    index->indexTarget = IS_CLOSED;
    index->errorFlag   = true;
    spin_unlock(&index->stateLock);
    logInfo("Setting UDS index target state to error");
    spin_lock(&index->stateLock);
  }
  // ASSERTION: On success, we leave in IS_OPENED state.
  // ASSERTION: On failure, we leave in IS_CLOSED state.
}

/*****************************************************************************/
static void changeDedupeState(struct kvdo_work_item *item)
{
  struct dedupe_index *index = container_of(item,
                                            struct dedupe_index,
                                            workItem);
  spin_lock(&index->stateLock);

  // Loop until the index is in the target state and the create flag is clear.
  while ((index->indexState != index->indexTarget) || index->createFlag) {
    if (index->indexState == IS_OPENED) {
      closeSession(index);
    } else {
      openSession(index);
    }
  }
  index->changing = false;
  index->deduping = index->dedupeFlag && (index->indexState == IS_OPENED);
  spin_unlock(&index->stateLock);
}

/*****************************************************************************/
static void setTargetState(struct dedupe_index *index,
                           IndexState           target,
                           bool                 changeDedupe,
                           bool                 dedupe,
                           bool                 setCreate)
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
    setup_work_item(&index->workItem, changeDedupeState, NULL, UDS_Q_ACTION);
    enqueue_work_queue(index->udsQueue, &index->workItem);
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
void dumpDedupeIndex(struct dedupe_index *index, bool showQueue)
{
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
    dump_work_queue(index->udsQueue);
  }
}

/*****************************************************************************/
void finishDedupeIndex(struct dedupe_index *index)
{
  setTargetState(index, IS_CLOSED, false, false, false);
  udsFreeConfiguration(index->configuration);
  finish_work_queue(index->udsQueue);
}

/*****************************************************************************/
void freeDedupeIndex(struct dedupe_index **indexPtr)
{
  if (*indexPtr == NULL) {
    return;
  }
  struct dedupe_index *index = *indexPtr;
  *indexPtr = NULL;

  free_work_queue(&index->udsQueue);
  stopPeriodicEventReporter(&index->timeoutReporter);
  spin_lock_bh(&index->pendingLock);
  if (index->startedTimer) {
    del_timer_sync(&index->pendingTimer);
  }
  spin_unlock_bh(&index->pendingLock);
  kobject_put(&index->dedupeObject);
}

/*****************************************************************************/
const char *getDedupeStateName(struct dedupe_index *index)
{
  spin_lock(&index->stateLock);
  const char *state = indexStateToString(index, index->indexState);
  spin_unlock(&index->stateLock);
  return state;
}

/*****************************************************************************/
void getIndexStatistics(struct dedupe_index *index, IndexStatistics *stats)
{
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
    result = udsGetIndexSessionStats(index->indexSession, &contextStats);
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
int messageDedupeIndex(struct dedupe_index *index, const char *name)
{
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
void postDedupeAdvice(struct data_kvio *dataKVIO)
{
  enqueueIndexOperation(dataKVIO, UDS_POST);
}

/*****************************************************************************/
void queryDedupeAdvice(struct data_kvio *dataKVIO)
{
  enqueueIndexOperation(dataKVIO, UDS_QUERY);
}

/*****************************************************************************/
void startDedupeIndex(struct dedupe_index *index, bool createFlag)
{
  setTargetState(index, IS_OPENED, true, true, createFlag);
}

/*****************************************************************************/
void stopDedupeIndex(struct dedupe_index *index)
{
  setTargetState(index, IS_CLOSED, false, false, false);
}

/*****************************************************************************/
void updateDedupeAdvice(struct data_kvio *dataKVIO)
{
  enqueueIndexOperation(dataKVIO, UDS_UPDATE);
}

/*****************************************************************************/
static void dedupeKobjRelease(struct kobject *kobj)
{
  struct dedupe_index *index = container_of(kobj,
                                            struct dedupe_index,
                                            dedupeObject);
  FREE(index->indexName);
  FREE(index);
}

/*****************************************************************************/
static ssize_t dedupeStatusShow(struct kobject   *kobj,
                                struct attribute *attr,
                                char             *buf)
{
  UDSAttribute *ua = container_of(attr, UDSAttribute, attr);
  struct dedupe_index *index = container_of(kobj,
                                            struct dedupe_index,
                                            dedupeObject);
  if (ua->showString != NULL) {
    return sprintf(buf, "%s\n", ua->showString(index));
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
  .showString = getDedupeStateName,
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
  struct dedupe_index *index = ptr;
  registerAllocatingThread(&index->allocatingThread, NULL);
}

/*****************************************************************************/
static void finishUDSQueue(void *ptr)
{
  unregisterAllocatingThread();
}

/*****************************************************************************/
int makeDedupeIndex(struct dedupe_index **indexPtr, KernelLayer *layer)
{
  setAlbireoTimeoutInterval(albireoTimeoutInterval);
  setMinAlbireoTimerInterval(minAlbireoTimerInterval);

  struct dedupe_index *index;
  int result = ALLOCATE(1, struct dedupe_index, "UDS index data", &index);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = allocSprintf("index name", &index->indexName,
                        "dev=%s offset=4096 size=%llu",
                        layer->deviceConfig->parent_device_name,
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
  result = make_work_queue(layer->threadNamePrefix, "dedupeQ",
                           &layer->wqDirectory, layer, index,
                           &udsQueueType, 1, NULL,
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
    free_work_queue(&index->udsQueue);
    udsFreeConfiguration(index->configuration);
    FREE(index->indexName);
    FREE(index);
    return result;
  }

  INIT_LIST_HEAD(&index->pendingHead);
  spin_lock_init(&index->pendingLock);
  spin_lock_init(&index->stateLock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
  timer_setup(&index->pendingTimer, timeoutIndexOperations, 0);
#else
  setup_timer(&index->pendingTimer, timeoutIndexOperations,
              (unsigned long) index);
#endif

  // UDS Timeout Reporter
  initPeriodicEventReporter(&index->timeoutReporter,
                            "UDS index timeout on %llu requests",
                            DEDUPE_TIMEOUT_REPORT_INTERVAL, layer);

  *indexPtr = index;
  return VDO_SUCCESS;
}
