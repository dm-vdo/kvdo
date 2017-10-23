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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/udsIndex.c#1 $
 */

#include "udsIndex.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"
#include "uds-block.h"

/**********************************************************************/

typedef struct udsAttribute {
  struct attribute attr;
  const char *(*showString)(DedupeIndex *);
} UDSAttribute;

/**********************************************************************/

enum { UDS_Q_ACTION };

static const KvdoWorkQueueType udsQueueType = {
  .needTimeouts = true,
  .actionTable  = {
    { .name = "uds_action", .code = UDS_Q_ACTION, .priority = 0 },
  },
};

/**********************************************************************/

// These are the values in the atomic dedupeContext.requestState field
enum {
  // The UdsRequest object is not in use.
  UR_IDLE = 0,
  // The UdsRequest object is in use, and VDO is waiting for the result.
  UR_BUSY = 1,
  // The UdsRequest object is in use, but has timed out.
  UR_TIMED_OUT = 2,
};

/**********************************************************************/

typedef struct udsIndex {
  DedupeIndex        common;
  struct kobject     dedupeObject;
  KvdoWorkItem       workItem;
  char              *indexName;
  UdsBlockContext    blockContext;
  UdsIndexSession    indexSession;
  atomic_t           active;
  bool               haveBlockContext;
  bool               haveIndexSession;
  // This spinlock protects the state fields and the starting of dedupe
  // requests.
  spinlock_t         stateLock;
  KvdoWorkQueue     *udsQueue;      // protected by stateLock
  const char        *dedupeState;   // protected by stateLock
  const char        *targetState;   // protected by stateLock
  unsigned int       maximum;       // protected by stateLock
  // This spinlock protects the pending list, the pending flag in each
  // KVIO, and the timeout list.
  spinlock_t         pendingLock;
  struct list_head   pendingHead;   // protected by pendingLock
  struct timer_list  pendingTimer;  // protected by pendingLock
  bool               startedTimer;  // protected by pendingLock
} UDSIndex;

// For dedupeState, ERROR means that we had an error changing state.
static const char *const ERROR = "error";

// For dedupeState, CLOSED means that there is no open block context.
static const char *const CLOSED = "CLOSED";

// For dedupeState, OPENING means that we are opening a block context.
static const char *const OPENING = "opening";

// For dedupeState, CLOSING means that we are closing a block context.
static const char *const CLOSING = "closing";

// For dedupeState, OFFLINE means that there is an open block context, but we
//   are not sending dedupe requests to the index.
static const char *const OFFLINE = "offline";

// For dedupeState, ONLINE means that there is an open block context, and we.
//   are sending dedupe requests to the index.
static const char *const ONLINE = "online";

/**********************************************************************/

// Version 1:  user space albireo index (limited to 32 bytes)
// Version 2:  kernel space albireo index (limited to 16 bytes)
enum { UDS_ADVICE_VERSION = 2 };

typedef struct udsAdvice {
  byte                version;
  byte                state;
  PhysicalBlockNumber pbn;
} __attribute__((__packed__)) UDSAdvice;

/**********************************************************************/
static inline bool stateIn2(const char *state, const char *s1, const char *s2)
{
  return (state == s1) || (state == s2);
}

/**********************************************************************/
static inline bool stateIn3(const char *state,
                            const char *s1,
                            const char *s2,
                            const char *s3)
{
  return (state == s1) || (state == s2) || (state == s3);
}

/**********************************************************************/
static void finishIndexOperation(UdsRequest *udsRequest)
{
  DataKVIO *dataKVIO = container_of(udsRequest, DataKVIO,
                                    dedupeContext.udsRequest);
  DedupeContext *dedupeContext = &dataKVIO->dedupeContext;
  if (compareAndSwap32(&dedupeContext->requestState, UR_BUSY, UR_IDLE)) {
    KVIO *kvio = dataKVIOAsKVIO(dataKVIO);
    DataVIO *dataVIO = vioAsDataVIO(kvio->vio);
    UDSIndex *index = container_of(kvio->layer->dedupeIndex, UDSIndex, common);

    spin_lock_bh(&index->pendingLock);
    if (dedupeContext->isPending) {
      list_del(&dedupeContext->pendingList);
      dedupeContext->isPending = false;
    }
    spin_unlock_bh(&index->pendingLock);

    dedupeContext->status = udsRequest->status;
    if ((udsRequest->type == UDS_POST) || (udsRequest->type == UDS_QUERY)) {
      dataVIO->isDuplicate
        = (udsRequest->status == UDS_SUCCESS) && udsRequest->found;
      if (dataVIO->isDuplicate) {
        UDSAdvice *advice = (UDSAdvice *) &udsRequest->oldMetadata;
        DataLocation location = (DataLocation) {
          .pbn   = advice->pbn,
          .state = advice->state,
        };
        setDedupeAdvice(&dataKVIO->dedupeContext, &location);
      }
    }
    invokeDedupeCallback(dataKVIO);
    atomic_dec(&index->active);
  } else {
    compareAndSwap32(&dedupeContext->requestState, UR_TIMED_OUT, UR_IDLE);
  }
}

/**********************************************************************/
static void startExpirationTimer(UDSIndex *index, DataKVIO *dataKVIO)
{
  if (!index->startedTimer) {
    index->startedTimer = true;
    mod_timer(&index->pendingTimer,
              getAlbireoTimeout(dataKVIO->dedupeContext.submissionTime));
  }
}

/**********************************************************************/
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

/**********************************************************************/
static void timeoutIndexOperations(unsigned long arg)
{
  UDSIndex *index = (UDSIndex *) arg;
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

/**********************************************************************/
static void enqueueIndexOperation(DataKVIO        *dataKVIO,
                                  UdsCallbackType  operation)
{
  KVIO *kvio = dataKVIOAsKVIO(dataKVIO);
  DedupeContext *dedupeContext = &dataKVIO->dedupeContext;
  UDSIndex *index = container_of(kvio->layer->dedupeIndex, UDSIndex, common);
  dedupeContext->status         = UDS_SUCCESS;
  dedupeContext->submissionTime = jiffies;
  dataKVIO->dataVIO.isDuplicate = false;
  if (compareAndSwap32(&dedupeContext->requestState, UR_IDLE, UR_BUSY)) {
    UdsRequest *udsRequest = &dataKVIO->dedupeContext.udsRequest;
    UDSAdvice advice = {
      .version = UDS_ADVICE_VERSION,
      .state   = dataKVIO->dataVIO.newMapped.state,
      .pbn     = kvio->vio->physical,
    };
    udsRequest->chunkName = *dedupeContext->chunkName;
    udsRequest->callback  = finishIndexOperation;
    udsRequest->context   = index->blockContext;
    udsRequest->type      = operation;
    udsRequest->update    = true;
    memcpy(&udsRequest->newMetadata, &advice, sizeof(advice));
    setupWorkItem(&kvio->enqueueable.workItem, startIndexOperation, NULL,
                  UDS_Q_ACTION);
    spin_lock(&index->stateLock);
    if ((index->dedupeState == ONLINE) && (index->targetState == NULL)) {
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
  }
  if (kvio != NULL) {
    invokeDedupeCallback(dataKVIO);
  }
}

/**********************************************************************/
static void changeDedupeState(KvdoWorkItem *item)
{
  UDSIndex *index = container_of(item, UDSIndex, workItem);
  int result = UDS_SUCCESS;
  for (bool workToDo = true; workToDo;) {
    // Under the stateLock, advance to the next dedupeState and targetState.
    spin_lock(&index->stateLock);
    if (result != UDS_SUCCESS) {
      result = UDS_SUCCESS;
      index->targetState = ERROR;
    }
    if (stateIn2(index->targetState, ONLINE, OFFLINE)) {
      // Pick the next state to drive us to being online/offline
      if (stateIn3(index->dedupeState, CLOSING, CLOSED, ERROR)) {
        index->dedupeState = OPENING;
      } else if (stateIn3(index->dedupeState, OPENING, OFFLINE, ONLINE)) {
        index->dedupeState = index->targetState;
      } else {
        index->dedupeState = ERROR;
      }
    } else if (stateIn2(index->targetState, CLOSED, ERROR)) {
      // Pick the next state to drive us to being closed
      if (stateIn3(index->dedupeState, OPENING, OFFLINE, ONLINE)) {
        index->dedupeState = CLOSING;
      } else if (stateIn3(index->dedupeState, CLOSING, CLOSED, ERROR)) {
        index->dedupeState = index->targetState;
      } else {
        index->dedupeState = ERROR;
      }
    } else {
      index->dedupeState = ERROR;
    }
    if (stateIn2(index->dedupeState, index->targetState, ERROR)) {
      // We have arrived in either the target state or the error state
      index->targetState = NULL;
      workToDo = false;
    }
    const char *dedupeState = index->dedupeState;
    spin_unlock(&index->stateLock);

    if (dedupeState == ERROR) {
      // We do logging while not holding the spinlock
      if (result == UDS_SUCCESS) {
        logWarning("setting UDS index target state to ERROR");
      } else {
        logWarningWithStringError(result,
                                  "setting UDS index target state to ERROR");
      }
    } else if (dedupeState == OPENING) {
      // Do the UDS calls to open the index
      if (!index->haveIndexSession) {
        result = udsRebuildLocalIndex(index->indexName, &index->indexSession);
      }
      if (result == UDS_SUCCESS) {
        index->haveIndexSession = true;
        result = udsOpenBlockContext(index->indexSession, sizeof(UDSAdvice),
                                     &index->blockContext);
      }
      if (result == UDS_SUCCESS) {
        index->haveBlockContext = true;
      }
    } else if (dedupeState == CLOSING) {
      // Do the UDS calls to close the index
      if (index->haveBlockContext) {
        result = udsCloseBlockContext(index->blockContext);
        index->haveBlockContext = false;
      }
      if (index->haveIndexSession) {
        int result2 = udsCloseIndexSession(index->indexSession);
        index->haveIndexSession = false;
        if (result == UDS_SUCCESS) {
          result = result2;
        }
      }
    }
  }
}

/**********************************************************************/
static void setTargetState(UDSIndex *index, const char *target)
{
  logInfo("Setting UDS index target state to %s", target);
  spin_lock(&index->stateLock);
  if (index->targetState != NULL) {
    // A change is already in progress, just change the target of the change
    index->targetState = target;
  } else if (((index->dedupeState == OFFLINE) && (target == ONLINE))
             || ((index->dedupeState == ONLINE) && (target == OFFLINE))) {
    // Online vs. offline changes can happen immediately
    index->dedupeState = target;
  } else if (target != index->dedupeState) {
    // Must start a change to a new state by enqueuing a work item that calls
    // changeDedupeState.
    index->targetState = target;
    setupWorkItem(&index->workItem, changeDedupeState, NULL, UDS_Q_ACTION);
    enqueueWorkQueue(index->udsQueue, &index->workItem);
  }
  spin_unlock(&index->stateLock);
}

/**********************************************************************/

/**********************************************************************/
static void dumpUDSIndex(DedupeIndex *dedupeIndex, bool showQueue)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  spin_lock(&index->stateLock);
  const char *state = index->dedupeState;
  const char *target = index->targetState;
  spin_unlock(&index->stateLock);
  logInfo("UDS index: state: %s", state);
  if (target != NULL) {
    logInfo("UDS index: changing to state: %s", target);
  }
  if (showQueue) {
    dumpWorkQueue(index->udsQueue);
  }
}

/**********************************************************************/
static void finishUDSIndex(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  setTargetState(index, CLOSED);
  finishWorkQueue(index->udsQueue);
}

/**********************************************************************/
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

/**********************************************************************/
static const char *getUDSStateName(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  return index->dedupeState;
}

/**********************************************************************/
static unsigned int getMaximumOutstandingUDS(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  return index->maximum;
};

/**********************************************************************/
static unsigned int getNumberOutstandingUDS(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  return atomic_read(&index->active);
};

/**********************************************************************/
static int processMessage(DedupeIndex *dedupeIndex, const char *name)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  if (strcasecmp(name, "index-disable") == 0) {
    setTargetState(index, OFFLINE);
    return 0;
  } else if (strcasecmp(name, "index-enable") == 0) {
    setTargetState(index, ONLINE);
    return 0;
  }
  return -EINVAL;
}

/**********************************************************************/
static void udsPost(DataKVIO *dataKVIO)
{
  enqueueIndexOperation(dataKVIO, UDS_POST);
}

/**********************************************************************/
static void udsQuery(DataKVIO *dataKVIO)
{
  enqueueIndexOperation(dataKVIO, UDS_QUERY);
}

/**********************************************************************/
static void startUDSIndex(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  setTargetState(index, ONLINE);
}

/**********************************************************************/
static void stopUDSIndex(DedupeIndex *dedupeIndex)
{
  UDSIndex *index = container_of(dedupeIndex, UDSIndex, common);
  setTargetState(index, CLOSED);
}

/**********************************************************************/
static void udsUpdate(DataKVIO *dataKVIO)
{
  enqueueIndexOperation(dataKVIO, UDS_UPDATE);
}

/**********************************************************************/
static void dedupeKobjRelease(struct kobject *kobj)
{
  UDSIndex *index = container_of(kobj, UDSIndex, dedupeObject);
  FREE(index->indexName);
  FREE(index);
}

/**********************************************************************/
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

/**********************************************************************/
static ssize_t dedupeStatusStore(struct kobject   *kobj,
                                 struct attribute *attr,
                                 const char       *buf,
                                 size_t            length)
{
  return -EINVAL;
}

/**********************************************************************/

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

/**********************************************************************/
int makeUDSIndex(KernelLayer *layer, DedupeIndex **indexPtr)
{
  UDSIndex *index;
  int result = ALLOCATE(1, UDSIndex, "UDS index data", &index);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(strlen(layer->deviceConfig->parentDeviceName) + 50, char,
                    "UDS index name", &index->indexName);
  if (result < 0) {
    FREE(index);
    return result;
  }

  VolumeGeometry *geometry = layer->geometry;
  BlockCount indexBlocks = (geometry->partitions[DATA_REGION].startBlock
                            - geometry->partitions[INDEX_REGION].startBlock);
  result = fixedSprintf("index name", index->indexName, VDO_BLOCK_SIZE,
                        UDS_INDEX_PATH_TOO_LONG, "dev=%s offset=4096 size=%"
                        PRIu64, layer->deviceConfig->parentDeviceName,
                        indexBlocks * VDO_BLOCK_SIZE);
  if (result < 0) {
    logError("Creating index name failed (%d)", result);
    FREE(index->indexName);
    FREE(index);
    return result;
  }

  result = makeWorkQueue(layer->threadNamePrefix, "DedupeQ",
                         &layer->wqDirectory, index, &udsQueueType, 1,
                         &index->udsQueue);
  if (result < 0) {
    logError("UDS index queue initialization failed (%d)", result);
    FREE(index->indexName);
    FREE(index);
    return result;
  }

  kobject_init(&index->dedupeObject, &dedupeKobjType);
  result = kobject_add(&index->dedupeObject, &layer->kobj, "dedupe");
  if (result != VDO_SUCCESS) {
    freeWorkQueue(&index->udsQueue);
    FREE(index->indexName);
    FREE(index);
    return result;
  }

  index->common.dump                      = dumpUDSIndex;
  index->common.free                      = freeUDSIndex;
  index->common.getDedupeStateName        = getUDSStateName;
  index->common.getMaximumOutstanding     = getMaximumOutstandingUDS;
  index->common.getNumberOutstanding      = getNumberOutstandingUDS;
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
  setup_timer(&index->pendingTimer, timeoutIndexOperations,
              (unsigned long) index);
  index->dedupeState  = CLOSED;
  index->targetState  = NULL;

  *indexPtr = &index->common;
  return VDO_SUCCESS;
}
