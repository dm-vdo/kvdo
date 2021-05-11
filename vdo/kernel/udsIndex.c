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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/udsIndex.c#16 $
 */

#include "udsIndex.h"

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
  // The UDS index session is opening or closing
  IS_CHANGING = 1,
  // The UDS index is open.  There is a UDS index session.
  IS_OPENED = 2,
} IndexState;

/*****************************************************************************/

typedef struct udsIndex {
  DedupeIndex        common;
  struct kobject     dedupeObject;
  RegisteredThread   allocatingThread;
  char              *indexName;
  UdsConfiguration   configuration;
  struct uds_parameters udsParams;
  struct uds_index_session *indexSession;
  atomic_t           active;
  // This spinlock protects the state fields and the starting of dedupe
  // requests.
  spinlock_t         stateLock;
  KvdoWorkItem       workItem;    // protected by stateLock
  KvdoWorkQueue     *udsQueue;    // protected by stateLock
  unsigned int       maximum;     // protected by stateLock
  IndexState         indexState;  // protected by stateLock
  IndexState         indexTarget; // protected by stateLock
  bool               changing;    // protected by stateLock
  bool               createFlag;  // protected by stateLock
  bool               dedupeFlag;  // protected by stateLock
  bool               deduping;    // protected by stateLock
  bool               errorFlag;   // protected by stateLock
  bool               suspended;   // protected by stateLock
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
static const char *CLOSED    = "closed";
static const char *CLOSING   = "closing";
static const char *ERROR     = "error";
static const char *OFFLINE   = "offline";
static const char *ONLINE    = "online";
static const char *OPENING   = "opening";
static const char *SUSPENDED = "suspended";
static const char *UNKNOWN   = "unknown";

/*****************************************************************************/
static const char *indexStateToString(UDSIndex *index, IndexState state)
{
  if (index->suspended) {
    return SUSPENDED;
  }

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
  struct udsChunkData *encoding = &request->newMetadata;
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
  const struct udsChunkData *encoding = &request->oldMetadata;
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
    udsRequest->session   = index->indexSession;
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
static void closeIndex(UDSIndex *index)
{
  // Change the index state so that getIndexStatistics will not try to
  // use the index session we are closing.
  index->indexState = IS_CHANGING;
  spin_unlock(&index->stateLock);
  int result = udsCloseIndex(index->indexSession);
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
static void openIndex(UDSIndex *index)
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

  int result = udsOpenIndex(createFlag ? UDS_CREATE : UDS_LOAD,
                            index->indexName, &index->udsParams,
                            index->configuration, index->indexSession);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Error opening index %s",
                            index->indexName);
  }
  spin_lock(&index->stateLock);
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
    index->indexState  = IS_OPENED;
  } else {
    index->indexState  = IS_CLOSED;
    index->indexTarget = IS_CLOSED;
    index->errorFlag   = true;
    spin_unlock(&index->stateLock);
    logInfo("Setting UDS index target state to error");
    spin_lock(&index->stateLock);
  }
  // ASSERTION: On success, we leave in IS_OPEN state.
  // ASSERTION: On failure, we leave in IS_CLOSED state.
}

/*****************************************************************************/
static void changeDedupeState(KvdoWorkItem *item)
{
  UDSIndex *index = container_of(item, UDSIndex, workItem);
  spin_lock(&index->stateLock);
  // Loop until the index is in the target state and the create flag is
  // clear.
  while (!index->suspended &&
         ((index->indexState != index->indexTarget) ||
          index->createFlag)) {
    if (index->indexState == IS_OPENED) {
      closeIndex(index);
    } else {
      openIndex(index);
    }
  }
  index->changing = false;
  index->deduping = index->dedupeFlag && (index->indexState == IS_OPENED);
  spin_unlock(&index->stateLock);
}


/*****************************************************************************/
static void launchDedupeStateChange(UDSIndex *index)
{
  // ASSERTION: We enter with the state_lock held.
  if (index->changing || index->suspended) {
    // Either a change is already in progress, or changes are
    // not allowed.
    return;
  }

  if (index->createFlag ||
      (index->indexState != index->indexTarget)) {
    index->changing = true;
    index->deduping = false;
    setupWorkItem(&index->workItem,
                  changeDedupeState,
                  NULL,
                  UDS_Q_ACTION);
    enqueueWorkQueue(index->udsQueue, &index->workItem);
    return;
  }

  // Online vs. offline changes happen immediately
  index->deduping = (index->dedupeFlag && !index->suspended &&
                     (index->indexState == IS_OPENED));

  // ASSERTION: We exit with the state_lock held.
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
  index->indexTarget = target;
  launchDedupeStateChange(index);
  const char *newState = indexStateToString(index, index->indexTarget);
  spin_unlock(&index->stateLock);
  if (oldState != newState) {
    logInfo("Setting UDS index target state to %s", newState);
  }
}

/*****************************************************************************/
static void suspendUDSIndex(DedupeIndex *dedupeIndex, bool saveFlag)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  spin_lock(&index->stateLock);
  index->suspended = true;
  IndexState indexState = index->indexState;
  spin_unlock(&index->stateLock);
  if (indexState != IS_CLOSED) {
    int result = udsSuspendIndexSession(index->indexSession, saveFlag);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "Error suspending dedupe index");
    }
  }
}

/*****************************************************************************/
static void resumeUDSIndex(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  int result = udsResumeIndexSession(index->indexSession);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Error resuming dedupe index");
  }
  spin_lock(&index->stateLock);
  index->suspended = false;
  launchDedupeStateChange(index);
  spin_unlock(&index->stateLock);
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
  udsDestroyIndexSession(index->indexSession);
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
static int processMessage(DedupeIndex *dedupeIndex, const char *name)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  if (strcasecmp(name, "index-close") == 0) {
    setTargetState(index, IS_CLOSED, false, false, false);
    return 0;
  } else if (strcasecmp(name, "index-create") == 0) {
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
  udsFreeConfiguration(index->configuration);
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
int makeUDSIndex(KernelLayer *layer, DedupeIndex **indexPtr)
{
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

  index->udsParams = (struct uds_parameters) UDS_PARAMETERS_INITIALIZER;
  indexConfigToUdsParameters(&layer->geometry.indexConfig, &index->udsParams);
  result = indexConfigToUdsConfiguration(&layer->geometry.indexConfig,
                                         &index->configuration);
  if (result != VDO_SUCCESS) {
    FREE(index->indexName);
    FREE(index);
    return result;
  }
  udsConfigurationSetNonce(index->configuration,
                           (UdsNonce) layer->geometry.nonce);

  result = udsCreateIndexSession(&index->indexSession);
  if (result != UDS_SUCCESS) {
    udsFreeConfiguration(index->configuration);
    FREE(index->indexName);
    FREE(index);
    return result;
  }

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
    udsDestroyIndexSession(index->indexSession);
    udsFreeConfiguration(index->configuration);
    FREE(index->indexName);
    FREE(index);
    return result;
  }

  kobject_init(&index->dedupeObject, &dedupeKobjType);
  result = kobject_add(&index->dedupeObject, &layer->kobj, "dedupe");
  if (result != VDO_SUCCESS) {
    freeWorkQueue(&index->udsQueue);
    udsDestroyIndexSession(index->indexSession);
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
  index->common.resume                    = resumeUDSIndex;
  index->common.start                     = startUDSIndex;
  index->common.stop                      = stopUDSIndex;
  index->common.suspend                   = suspendUDSIndex;
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
