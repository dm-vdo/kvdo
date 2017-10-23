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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/kernelVDO.c#1 $
 */

#include "kernelVDOInternals.h"

#include <linux/delay.h>

#include "memoryAlloc.h"

#include "statistics.h"
#include "threadConfig.h"
#include "vdo.h"
#include "vdoClose.h"
#include "vdoDebug.h"
#include "vdoLoad.h"
#include "vdoResize.h"
#include "vdoResizeLogical.h"

#include "kernelLayer.h"
#include "kvio.h"
#include "logger.h"

enum { PARANOID_THREAD_CONSISTENCY_CHECKS = 0 };

/**
 * Run work items in each worker thread to stop the heartbeats.
 *
 * @param kvdo      The KVDO object containing the worker threads
 **/
static void stopAllHeartbeats(KVDO *kvdo);

/**********************************************************************/
static void startKVDORequestQueue(void *ptr)
{
  KVDOThread  *thread = ptr;
  KVDO        *kvdo   = thread->kvdo;
  KernelLayer *layer  = container_of(kvdo, KernelLayer, kvdo);
  registerAllocatingThread(&thread->allocatingThread,
                           &layer->allocationsAllowed);
  setWorkQueuePrivateData(thread);
}

/**********************************************************************/
static void finishKVDORequestQueue(void *ptr)
{
  unregisterAllocatingThread();
}

/**********************************************************************/
static const KvdoWorkQueueType requestQueueType = {
  .start       = startKVDORequestQueue,
  .finish      = finishKVDORequestQueue,
  .actionTable = {
    { .name = "req_completion",
      .code = REQ_Q_ACTION_COMPLETION,
      .priority = 1 },
    { .name = "req_flush",
      .code = REQ_Q_ACTION_FLUSH,
      .priority = 2 },
    { .name = "req_heartbeat",
      .code = REQ_Q_ACTION_HEARTBEAT,
      .priority = 2 },
    { .name = "req_map_bio",
      .code = REQ_Q_ACTION_MAP_BIO,
      .priority = 0 },
    { .name = "req_shutdown",
      .code = REQ_Q_ACTION_SHUTDOWN,
      .priority = 0 },
    { .name = "req_sync",
      .code = REQ_Q_ACTION_SYNC,
      .priority = 2 },
    { .name = "req_vio_callback",
      .code = REQ_Q_ACTION_VIO_CALLBACK,
      .priority = 1 },
  },
};

/**********************************************************************/
int initializeKVDO(KVDO                *kvdo,
                   const ThreadConfig  *threadConfig,
                   char               **reason)
{
  unsigned int baseThreads = threadConfig->baseThreadCount;
  int result = ALLOCATE(baseThreads, KVDOThread,
                        "request processing work queue",
                        &kvdo->threads);
  if (result != VDO_SUCCESS) {
    *reason = "Cannot allocation thread structures";
    return result;
  }
  KernelLayer *layer = container_of(kvdo, KernelLayer, kvdo);
  for (kvdo->initializedThreadCount = 0;
       kvdo->initializedThreadCount < baseThreads;
       kvdo->initializedThreadCount++) {
    KVDOThread *thread = &kvdo->threads[kvdo->initializedThreadCount];

    // Used when waiting on heartbeat stop
    init_completion(&thread->heartbeatWait);

    // Setup heartbeat data
    setupHeartbeat(&thread->heartbeat);

    thread->kvdo = kvdo;
    thread->threadID = kvdo->initializedThreadCount;

    char queueName[MAX_QUEUE_NAME_LEN];
    // Copy only LEN - 1 bytes and ensure NULL termination.
    getVDOThreadName(threadConfig, kvdo->initializedThreadCount,
                     queueName, sizeof(queueName));
    int result = makeWorkQueue(layer->threadNamePrefix, queueName,
                               &layer->wqDirectory, thread, &requestQueueType,
                               1, &thread->requestQueue);
    if (result != VDO_SUCCESS) {
      *reason = "Cannot initialize request queue";
      while (kvdo->initializedThreadCount > 0) {
        unsigned int threadToDestroy = kvdo->initializedThreadCount - 1;
        thread = &kvdo->threads[threadToDestroy];
        finishWorkQueue(thread->requestQueue);
        freeWorkQueue(&thread->requestQueue);
        kvdo->initializedThreadCount--;
      }
      FREE(kvdo->threads);
      return result;
    }

  }
  return VDO_SUCCESS;
}

/**********************************************************************/
int startKVDO(KVDO                 *kvdo,
              PhysicalLayer        *common,
              const VDOLoadConfig  *loadConfig,
              bool                  vioTraceRecording,
              char                **reason)
{
  KernelLayer *layer = asKernelLayer(common);
  init_completion(&layer->callbackSync);
  int result = performVDOLoad(kvdo->vdo, loadConfig);
  if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
    *reason = "Cannot load metadata from device";
    return result;
  }

  setVDOTracingFlags(kvdo->vdo, vioTraceRecording);

  for (int i = 0; i < kvdo->initializedThreadCount; i++) {
    startHeartbeat(&kvdo->threads[i].heartbeat);
    // Stagger the heartbeats a bit.
    msleep(50 / kvdo->initializedThreadCount);
  }
  kvdo->heartsBeating = true;

  return VDO_SUCCESS;
}

/**********************************************************************/
int stopKVDO(KVDO *kvdo)
{
  if (kvdo->vdo == NULL) {
    return VDO_SUCCESS;
  }

  // In case we haven't already been suspended.
  if (kvdo->heartsBeating) {
    stopAllHeartbeats(kvdo);
  }

  KernelLayer *layer = container_of(kvdo, KernelLayer, kvdo);
  init_completion(&layer->callbackSync);
  return performVDOClose(kvdo->vdo);
}

/**********************************************************************/
void finishKVDO(KVDO *kvdo)
{
  for (int i = 0; i < kvdo->initializedThreadCount; i++) {
    finishWorkQueue(kvdo->threads[i].requestQueue);
  }
}

/**********************************************************************/
void destroyKVDO(KVDO *kvdo)
{
  destroyVDO(kvdo->vdo);
  for (int i = 0; i < kvdo->initializedThreadCount; i++) {
    freeWorkQueue(&kvdo->threads[i].requestQueue);
  }
  FREE(kvdo->threads);
  kvdo->threads = NULL;
}


/**********************************************************************/
void dumpKVDOWorkQueue(KVDO *kvdo)
{
  for (int i = 0; i < kvdo->initializedThreadCount; i++) {
    dumpWorkQueue(kvdo->threads[i].requestQueue);
  }
}

/**********************************************************************/
typedef struct {
  KvdoWorkItem       workItem;
  KVDO              *kvdo;
  void              *data;
  struct completion *completion;
} SyncQueueWork;

/**
 * Initiate an arbitrary asynchronous base-code operation and wait for
 * it.
 *
 * An async queue operation is performed and we wait for completion.
 *
 * @param kvdo       The kvdo data handle
 * @param action     The operation to perform
 * @param data       Unique data that can be used by the operation
 * @param threadID   The thread on which to perform the operation
 * @param completion The completion to wait on
 *
 * @return VDO_SUCCESS of an error code
 **/
static void performKVDOOperation(KVDO              *kvdo,
                                 KvdoWorkFunction   action,
                                 void              *data,
                                 ThreadID           threadID,
                                 struct completion *completion)
{
  SyncQueueWork  sync;

  memset(&sync, 0, sizeof(sync));
  setupWorkItem(&sync.workItem, action, NULL, REQ_Q_ACTION_SYNC);
  sync.kvdo       = kvdo;
  sync.data       = data;
  sync.completion = completion;

  init_completion(completion);
  enqueueKVDOWork(kvdo, &sync.workItem, threadID);
  wait_for_completion(completion);
}

/**
 * Initiate an arbitrary asynchronous base-code operation in a
 * specific thread and wait for it.
 *
 * An async queue operation is performed and we wait for completion.
 *
 * The thread specification is used so that the caller can iterate
 * over all threads.
 *
 * @param thread     The thread in which to run the action
 * @param action     The operation to perform
 * @param data       Unique data that can be used by the operation
 * @param completion The completion to wait on
 *
 * @return VDO_SUCCESS of an error code
 **/
static void performKVDOThreadAction(KVDOThread        *thread,
                                    KvdoWorkFunction   action,
                                    void              *data,
                                    struct completion *completion)
{
  SyncQueueWork  sync;

  memset(&sync, 0, sizeof(sync));
  setupWorkItem(&sync.workItem, action, NULL, REQ_Q_ACTION_SYNC);
  sync.kvdo       = thread->kvdo;
  sync.data       = data;
  sync.completion = completion;

  init_completion(completion);
  enqueueWorkQueue(thread->requestQueue, &sync.workItem);
  wait_for_completion(completion);
}

/**********************************************************************/
static void stopHeartbeatWork(KvdoWorkItem *item)
{
  SyncQueueWork *suspend = container_of(item, SyncQueueWork, workItem);
  KVDOThread *thread = (KVDOThread *) suspend->data;
  // Remove the heartbeat timer here.  There is no race between stopHeartbeat()
  // and kvdoHeartbeatWork() because both are called on the same thread.
  stopHeartbeat(&thread->heartbeat);
}

/**********************************************************************/
static void stopAllHeartbeats(KVDO *kvdo)
{
  // Stop the heartbeat so no recycler work is done afterwards. This
  // must be done in the request queue thread so create a workitem and
  // throw it on the queue, then wait for heartbeat to stop before
  // doing rest of post suspend work
  for (int i = 0; i < kvdo->initializedThreadCount; i++) {
    performKVDOThreadAction(&kvdo->threads[i], stopHeartbeatWork,
                            &kvdo->threads[i],
                            &kvdo->threads[i].heartbeatWait);
  }
  kvdo->heartsBeating = false;
}

/**********************************************************************/
void suspendKVDO(KVDO *kvdo)
{
  stopAllHeartbeats(kvdo);
}

/**********************************************************************/
void resumeKVDO(KVDO *kvdo)
{
  for (int i = 0; i < kvdo->initializedThreadCount; i++) {
    startHeartbeat(&kvdo->threads[i].heartbeat);
  }
  kvdo->heartsBeating = true;
}

/**********************************************************************/
typedef struct {
  bool enable;
  bool wasEnabled;
} VDOCompressData;

/**
 * Does the work of calling the base code to set compress state, then
 * tells the function waiting on completion to go ahead.
 *
 * @param item  The work item
 **/
static void setCompressingWork(KvdoWorkItem *item)
{
  SyncQueueWork   *work  = container_of(item, SyncQueueWork, workItem);
  VDOCompressData *data  = (VDOCompressData *)work->data;
  data->wasEnabled = setVDOCompressing(getVDO(work->kvdo), data->enable);
  complete(work->completion);
}

/***********************************************************************/
bool setKVDOCompressing(KVDO *kvdo, bool enableCompression)
{
  struct completion compressWait;
  VDOCompressData data;
  data.enable = enableCompression;
  performKVDOOperation(kvdo, setCompressingWork, &data,
                       getPackerZoneThread(getThreadConfig(kvdo->vdo)),
                       &compressWait);
  return data.wasEnabled;
}

/**
 * Does the work of calling the vdo statistics gathering tool
 *
 * @param item   The work item
 **/
static void getVDOStatisticsWork(KvdoWorkItem *item)
{
  SyncQueueWork *work  = container_of(item, SyncQueueWork, workItem);
  VDOStatistics *stats = (VDOStatistics *)work->data;
  getVDOStatistics(getVDO(work->kvdo), stats);
  complete(work->completion);
}

/***********************************************************************/
void getKVDOStatistics(KVDO *kvdo, VDOStatistics *stats)
{
  struct completion statsWait;
  memset(stats, 0, sizeof(VDOStatistics));
  performKVDOOperation(kvdo, getVDOStatisticsWork, stats,
                       getAdminThread(getThreadConfig(kvdo->vdo)),
                       &statsWait);
}

/**
 * A structure to invoke an arbitrary VDO action.
 **/
typedef struct vdoActionData {
  VDOAction          *action;
  VDOCompletion      *vdoCompletion;
  struct completion   waiter;
} VDOActionData;

/**
 * Initialize a VDOActionData structure so that the specified action
 * can be invoked on the specified completion.
 *
 * @param data              A VDOActionData.
 * @param action            The VDOAction to execute.
 * @param vdoCompletion     The VDO completion upon which the action acts.
 **/
static void initializeVDOActionData(VDOActionData *data,
                                    VDOAction     *action,
                                    VDOCompletion *vdoCompletion)
{
  *data = (VDOActionData) {
    .action        = action,
    .vdoCompletion = vdoCompletion,
  };
}

/**
 * The VDO callback that completes the KVDO completion.
 *
 * @param vdoCompletion     The VDO completion which was acted upon.
 **/
static void finishVDOAction(VDOCompletion *vdoCompletion)
{
  SyncQueueWork *work = vdoCompletion->parent;
  complete(work->completion);
}

/**
 * Perform a VDO base code action as specified by a VDOActionData.
 *
 * Sets the completion callback and parent inside the VDOActionData
 * so that the corresponding kernel completion is completed when
 * the VDO completion is.
 *
 * @param item          A KVDO work queue item.
 **/
static void performVDOActionWork(KvdoWorkItem *item)
{
  SyncQueueWork *work = container_of(item, SyncQueueWork, workItem);
  VDOActionData *data = work->data;
  ThreadID       id   = getPhysicalLayer()->getCurrentThreadID();

  setCallbackWithParent(data->vdoCompletion, finishVDOAction, id, work);
  data->action(data->vdoCompletion);
}

/**********************************************************************/
int performKVDOExtendedCommand(KVDO *kvdo, int argc, char **argv)
{
  VDOActionData        data;
  VDOCommandCompletion cmd;

  int result = initializeVDOCommandCompletion(&cmd, getVDO(kvdo), argc, argv);
  if (result != VDO_SUCCESS) {
    return result;
  }

  initializeVDOActionData(&data, executeVDOExtendedCommand, &cmd.completion);
  performKVDOOperation(kvdo, performVDOActionWork, &data,
                       getAdminThread(getThreadConfig(kvdo->vdo)),
                       &data.waiter);

  return destroyVDOCommandCompletion(&cmd);
}

/**********************************************************************/
void heartbeatCallback(HeartbeatData *heartbeat, ThreadID threadID)
{
  KVDOThread *thread  = container_of(heartbeat, KVDOThread, heartbeat);
  beat(thread->kvdo->vdo, threadID);
}

/**********************************************************************/
void dumpKVDOStatus(KVDO *kvdo)
{
  dumpVDOStatus(kvdo->vdo);
}

/**********************************************************************/
bool getKVDOCompressing(KVDO *kvdo)
{
  return getVDOCompressing(kvdo->vdo);
}

/**********************************************************************/
int kvdoPrepareToGrowPhysical(KVDO *kvdo, BlockCount physicalCount)
{
  VDO *vdo = kvdo->vdo;
  return prepareToGrowPhysical(vdo, physicalCount);
}

/**********************************************************************/
int kvdoResizePhysical(KVDO *kvdo, BlockCount physicalCount)
{
  KernelLayer *layer = container_of(kvdo, KernelLayer, kvdo);
  init_completion(&layer->callbackSync);
  int result = performGrowPhysical(kvdo->vdo, physicalCount);
  if (result != VDO_SUCCESS) {
    logError("resize operation failed, result = %d", result);
    return result;
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int kvdoPrepareToGrowLogical(KVDO *kvdo, BlockCount logicalCount)
{
  VDO *vdo = kvdo->vdo;
  return prepareToGrowLogical(vdo, logicalCount);
}

/**********************************************************************/
int kvdoResizeLogical(KVDO *kvdo, BlockCount logicalCount)
{
  KernelLayer *layer = container_of(kvdo, KernelLayer, kvdo);
  init_completion(&layer->callbackSync);
  int result = performGrowLogical(kvdo->vdo, logicalCount);
  if (result != VDO_SUCCESS) {
    logError("grow logical operation failed, result = %d", result);
  }

  return result;
}

/**********************************************************************/
WritePolicy getKVDOWritePolicy(KVDO *kvdo)
{
  return getWritePolicy(kvdo->vdo);
}

/**********************************************************************/
void enqueueKVDOThreadWork(KVDOThread    *thread,
                           KvdoWorkItem  *item)
{
  enqueueWorkQueue(thread->requestQueue, item);
}

/**********************************************************************/
void enqueueKVDOThreadWorkDelayed(KVDOThread   *thread,
                                  KvdoWorkItem *item,
                                  Jiffies       executionTime)
{
  enqueueWorkQueueDelayed(thread->requestQueue, item, executionTime);
}

/**********************************************************************/
void enqueueKVDOWork(KVDO *kvdo, KvdoWorkItem *item, ThreadID threadID)
{
  enqueueKVDOThreadWork(&kvdo->threads[threadID], item);
}

/**********************************************************************/
void enqueueKVIO(KVIO             *kvio,
                 KvdoWorkFunction  work,
                 void             *statsFunction,
                 unsigned int      action)
{
  ThreadID threadID = vioAsCompletion(kvio->vio)->callbackThreadID;
  BUG_ON(threadID < 0);
  BUG_ON(threadID >= kvio->layer->kvdo.initializedThreadCount);
  launchKVIO(kvio, work, statsFunction, action,
             kvio->layer->kvdo.threads[threadID].requestQueue);
}

/**********************************************************************/
static void kvdoEnqueueWork(KvdoWorkItem *workItem)
{
  KvdoEnqueueable *kvdoEnqueueable = container_of(workItem,
                                                  KvdoEnqueueable,
                                                  workItem);
  runCallback(kvdoEnqueueable->enqueueable.completion);
}

/**********************************************************************/
void kvdoEnqueue(Enqueueable *enqueueable)
{
  KvdoEnqueueable *kvdoEnqueueable = container_of(enqueueable,
                                                  KvdoEnqueueable,
                                                  enqueueable);
  KernelLayer *layer    = asKernelLayer(enqueueable->completion->layer);
  ThreadID     threadID = enqueueable->completion->callbackThreadID;
  if (ASSERT(threadID < layer->kvdo.initializedThreadCount,
             "threadID %u (completion type %d) is less than thread count %u",
             threadID, enqueueable->completion->type,
             layer->kvdo.initializedThreadCount) != UDS_SUCCESS) {
    BUG();
  }

  if (enqueueable->completion->type == VIO_COMPLETION) {
    vioAddTraceRecord(asVIO(enqueueable->completion),
                      THIS_LOCATION("$F($cb)"));
  }
  setupWorkItem(&kvdoEnqueueable->workItem, kvdoEnqueueWork,
                (KvdoWorkFunction) enqueueable->completion->callback,
                REQ_Q_ACTION_COMPLETION);
  enqueueKVDOThreadWork(&layer->kvdo.threads[threadID],
                        &kvdoEnqueueable->workItem);
}

/**********************************************************************/
ThreadID kvdoGetCurrentThreadID(void)
{
  KVDOThread *thread = getWorkQueuePrivateData();
  if (thread == NULL) {
    return INVALID_THREAD_ID;
  }

  ThreadID threadID = thread->threadID;
  if (PARANOID_THREAD_CONSISTENCY_CHECKS) {
    KVDO        *kvdo        = thread->kvdo;
    KernelLayer *kernelLayer = asKernelLayer(getPhysicalLayer());
    BUG_ON(&kernelLayer->kvdo != kvdo);
    BUG_ON(threadID >= kvdo->initializedThreadCount);
    BUG_ON(thread != &kvdo->threads[threadID]);
  }
  return threadID;
}

/**********************************************************************/
static PhysicalLayer *getKernelPhysicalLayer(void)
{
  KVDOThread  *thread = getWorkQueuePrivateData();
  if (thread == NULL) {
    return NULL;
  }
  KVDO        *kvdo   = thread->kvdo;
  KernelLayer *layer  = container_of(kvdo, KernelLayer, kvdo);
  return &layer->common;
}

void initKernelVDOOnce(void)
{
  registerPhysicalLayerGetter(getKernelPhysicalLayer);
}
