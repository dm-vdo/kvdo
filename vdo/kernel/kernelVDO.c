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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.c#6 $
 */

#include "kernelVDOInternals.h"

#include <linux/delay.h>

#include "memoryAlloc.h"

#include "physicalLayer.h"
#include "statistics.h"
#include "threadConfig.h"
#include "threadData.h"
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

/**********************************************************************/
static void startKVDORequestQueue(void *ptr)
{
  struct kvdo_thread *thread = ptr;
  KVDO               *kvdo   = thread->kvdo;
  KernelLayer        *layer  = container_of(kvdo, KernelLayer, kvdo);
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
    { .name = "req_map_bio",
      .code = REQ_Q_ACTION_MAP_BIO,
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
  int result = ALLOCATE(baseThreads, struct kvdo_thread,
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
    struct kvdo_thread *thread = &kvdo->threads[kvdo->initializedThreadCount];

    thread->kvdo = kvdo;
    thread->threadID = kvdo->initializedThreadCount;

    char queueName[MAX_QUEUE_NAME_LEN];
    // Copy only LEN - 1 bytes and ensure NULL termination.
    getVDOThreadName(threadConfig, kvdo->initializedThreadCount,
                     queueName, sizeof(queueName));
    int result = makeWorkQueue(layer->threadNamePrefix, queueName,
                               &layer->wqDirectory, layer, thread,
                               &requestQueueType, 1, &thread->requestQueue);
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
  return VDO_SUCCESS;
}

/**********************************************************************/
int stopKVDO(KVDO *kvdo)
{
  if (kvdo->vdo == NULL) {
    return VDO_SUCCESS;
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
struct sync_queue_work {
  KvdoWorkItem       workItem;
  KVDO              *kvdo;
  void              *data;
  struct completion *completion;
};

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
  struct sync_queue_work  sync;

  memset(&sync, 0, sizeof(sync));
  setupWorkItem(&sync.workItem, action, NULL, REQ_Q_ACTION_SYNC);
  sync.kvdo       = kvdo;
  sync.data       = data;
  sync.completion = completion;

  init_completion(completion);
  enqueueKVDOWork(kvdo, &sync.workItem, threadID);
  wait_for_completion(completion);
}

/**********************************************************************/
struct vdo_compress_data {
  bool enable;
  bool wasEnabled;
};

/**
 * Does the work of calling the base code to set compress state, then
 * tells the function waiting on completion to go ahead.
 *
 * @param item  The work item
 **/
static void setCompressingWork(KvdoWorkItem *item)
{
  struct sync_queue_work   *work  = container_of(item,
                                                 struct sync_queue_work,
                                                 workItem);
  struct vdo_compress_data *data  = (struct vdo_compress_data *)work->data;
  data->wasEnabled = setVDOCompressing(getVDO(work->kvdo), data->enable);
  complete(work->completion);
}

/***********************************************************************/
bool setKVDOCompressing(KVDO *kvdo, bool enableCompression)
{
  struct completion compressWait;
  struct vdo_compress_data data;
  data.enable = enableCompression;
  performKVDOOperation(kvdo, setCompressingWork, &data,
                       getPackerZoneThread(getThreadConfig(kvdo->vdo)),
                       &compressWait);
  return data.wasEnabled;
}

/**********************************************************************/
struct vdo_read_only_data {
  int result;
};

/**********************************************************************/
static void enterReadOnlyModeWork(KvdoWorkItem *item)
{
  struct sync_queue_work    *work = container_of(item,
                                              struct sync_queue_work,
                                              workItem);
  struct vdo_read_only_data *data = work->data;
  makeVDOReadOnly(getVDO(work->kvdo), data->result);
  complete(work->completion);
}

/***********************************************************************/
void setKVDOReadOnly(KVDO *kvdo, int result)
{
  struct completion readOnlyWait;
  struct vdo_read_only_data data;
  data.result = result;
  performKVDOOperation(kvdo, enterReadOnlyModeWork, &data,
                       getAdminThread(getThreadConfig(kvdo->vdo)),
                       &readOnlyWait);
}

/**
 * Does the work of calling the vdo statistics gathering tool
 *
 * @param item   The work item
 **/
static void getVDOStatisticsWork(KvdoWorkItem *item)
{
  struct sync_queue_work *work = container_of(item,
                                               struct sync_queue_work,
                                               workItem);
  VDOStatistics          *stats = (VDOStatistics *)work->data;
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
struct vdo_action_data {
  VDOAction          *action;
  VDOCompletion      *vdoCompletion;
  struct completion   waiter;
};

/**
 * Initialize a struct vdo_action_data structure so that the specified
 * action can be invoked on the specified completion.
 *
 * @param data              A vdo_action_data structure.
 * @param action            The VDOAction to execute.
 * @param vdoCompletion     The VDO completion upon which the action acts.
 **/
static void initializeVDOActionData(struct vdo_action_data *data,
                                    VDOAction              *action,
                                    VDOCompletion          *vdoCompletion)
{
  *data = (struct vdo_action_data) {
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
  struct sync_queue_work *work = vdoCompletion->parent;
  complete(work->completion);
}

/**
 * Perform a VDO base code action as specified by a vdo_action_data
 * structure..
 *
 * Sets the completion callback and parent inside the vdo_action_data
 * structure so that the corresponding kernel completion is completed
 * when the VDO completion is.
 *
 * @param item          A KVDO work queue item.
 **/
static void performVDOActionWork(KvdoWorkItem *item)
{
  struct sync_queue_work *work = container_of(item,
                                              struct  sync_queue_work,
                                              workItem);
  struct vdo_action_data *data = work->data;
  ThreadID                id   = getCallbackThreadID();

  setCallbackWithParent(data->vdoCompletion, finishVDOAction, id, work);
  data->action(data->vdoCompletion);
}

/**********************************************************************/
int performKVDOExtendedCommand(KVDO *kvdo, int argc, char **argv)
{
  struct vdo_action_data data;
  VDOCommandCompletion   cmd;

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
void enqueueKVDOThreadWork(struct kvdo_thread *thread,
                           KvdoWorkItem       *item)
{
  enqueueWorkQueue(thread->requestQueue, item);
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
ThreadID getCallbackThreadID(void)
{
  struct kvdo_thread *thread = getWorkQueuePrivateData();
  if (thread == NULL) {
    return INVALID_THREAD_ID;
  }

  ThreadID threadID = thread->threadID;
  if (PARANOID_THREAD_CONSISTENCY_CHECKS) {
    KVDO        *kvdo        = thread->kvdo;
    KernelLayer *kernelLayer = container_of(kvdo, KernelLayer, kvdo);
    BUG_ON(&kernelLayer->kvdo != kvdo);
    BUG_ON(threadID >= kvdo->initializedThreadCount);
    BUG_ON(thread != &kvdo->threads[threadID]);
  }
  return threadID;
}
