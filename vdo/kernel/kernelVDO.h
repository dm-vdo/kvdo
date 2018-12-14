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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.h#3 $
 */

#ifndef KERNEL_VDO_H
#define KERNEL_VDO_H

#include "completion.h"
#include "kernelTypes.h"
#include "threadRegistry.h"
#include "workQueue.h"

typedef struct {
  KVDO              *kvdo;
  ThreadID           threadID;
  KvdoWorkQueue     *requestQueue;
  RegisteredThread   allocatingThread;
} KVDOThread;

struct kvdo {
  KVDOThread        *threads;
  ThreadID           initializedThreadCount;
  KvdoWorkItem       workItem;
  VDOAction         *action;
  VDOCompletion     *completion;
  // Base-code device info
  VDO               *vdo;
};

typedef enum reqQAction {
  REQ_Q_ACTION_COMPLETION,
  REQ_Q_ACTION_FLUSH,
  REQ_Q_ACTION_MAP_BIO,
  REQ_Q_ACTION_SYNC,
  REQ_Q_ACTION_VIO_CALLBACK
} ReqQAction;

/**
 * Initialize the base code interface.
 *
 * @param [in]  kvdo          The KVDO to be initialized
 * @param [in]  threadConfig  The base-code thread configuration
 * @param [out] reason        The reason for failure
 *
 * @return  VDO_SUCCESS or an error code
 **/
int initializeKVDO(KVDO                *kvdo,
                   const ThreadConfig  *threadConfig,
                   char               **reason);

/**
 * Starts the base VDO instance associated with the kernel layer
 *
 * @param [in]  kvdo                  The KVDO to be started
 * @param [in]  common                The physical layer pointer
 * @param [in]  loadConfig            Load-time parameters for the VDO
 * @param [in]  vioTraceRecording     Debug flag to store
 * @param [out] reason                The reason for failure
 *
 * @return VDO_SUCCESS if started, otherwise error
 */
int startKVDO(KVDO                 *kvdo,
              PhysicalLayer        *common,
              const VDOLoadConfig  *loadConfig,
              bool                  vioTraceRecording,
              char                **reason);

/**
 * Stops the base VDO instance associated with the kernel layer
 *
 * @param kvdo          The KVDO to be stopped
 *
 * @return VDO_SUCCESS if stopped, otherwise error
 */
int stopKVDO(KVDO *kvdo);

/**
 * Shut down the base code interface. The kvdo object must first be
 * stopped.
 *
 * @param kvdo         The KVDO to be shut down
 **/
void finishKVDO(KVDO *kvdo);

/**
 * Free up storage of the base code interface. The KVDO object must
 * first have been "finished".
 *
 * @param kvdo         The KVDO object to be destroyed
 **/
void destroyKVDO(KVDO *kvdo);


/**
 * Dump to the kernel log any work-queue info associated with the base
 * code.
 *
 * @param kvdo     The KVDO object to be examined
 **/
void dumpKVDOWorkQueue(KVDO *kvdo);

/**
 * Get the VDO pointer for a kvdo object
 *
 * @param kvdo          The KVDO object
 *
 * @return the VDO pointer
 */
static inline VDO *getVDO(KVDO *kvdo)
{
  return kvdo->vdo;
}

/**
 * Set whether compression is enabled.
 *
 * @param kvdo               The KVDO object
 * @param enableCompression  The new compression mode
 *
 * @return state of compression before new value is set
 **/
bool setKVDOCompressing(KVDO *kvdo, bool enableCompression);

/**
 * Get the current compression mode
 *
 * @param kvdo          The KVDO object to be queried
 *
 * @return whether compression is currently enabled
 */
bool getKVDOCompressing(KVDO *kvdo);

/**
 * Gets the latest statistics gathered by the base code.
 *
 * @param kvdo  the KVDO object
 * @param stats the statistics struct to fill in
 */
void getKVDOStatistics(KVDO *kvdo, VDOStatistics *stats);

/**
 * Get the current write policy
 *
 * @param kvdo          The KVDO to be queried
 *
 * @return  the write policy in effect
 */
WritePolicy getKVDOWritePolicy(KVDO *kvdo);

/**
 * Dump base code status information to the kernel log for debugging.
 *
 * @param kvdo          The KVDO to be examined
 */
void dumpKVDOStatus(KVDO *kvdo);

/**
 * Request the base code prepare to grow the physical space.
 *
 * @param kvdo           The KVDO to be updated
 * @param physicalCount  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdoPrepareToGrowPhysical(KVDO *kvdo, BlockCount physicalCount);

/**
 * Notify the base code of resized physical storage.
 *
 * @param kvdo           The KVDO to be updated
 * @param physicalCount  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdoResizePhysical(KVDO *kvdo, BlockCount physicalCount);

/**
 * Request the base code prepare to grow the logical space.
 *
 * @param kvdo          The KVDO to be updated
 * @param logicalCount  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdoPrepareToGrowLogical(KVDO *kvdo, BlockCount logicalCount);

/**
 * Request the base code grow the logical space.
 *
 * @param kvdo          The KVDO to be updated
 * @param logicalCount  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdoResizeLogical(KVDO *kvdo, BlockCount logicalCount);

/**
 * Request the base code go read-only.
 *
 * @param kvdo          The KVDO to be updated
 * @param result        The error code causing the read only
 */
void setKVDOReadOnly(KVDO *kvdo, int result);

/**
 * Perform an extended base-code command
 *
 * @param kvdo          The KVDO upon which to perform the operation.
 * @param argc          The number of arguments to the command.
 * @param argv          The command arguments. Note that all extended
 *                        command argv[0] strings start with "x-".
 *
 * @return VDO_SUCCESS or an error code
 **/
int performKVDOExtendedCommand(KVDO *kvdo, int argc, char **argv);

/**
 * Enqueue a work item to be processed in the base code context.
 *
 * @param kvdo         The KVDO object in which to run the work item
 * @param item         The work item to be run
 * @param threadID     The thread on which to run the work item
 **/
void enqueueKVDOWork(KVDO *kvdo, KvdoWorkItem *item, ThreadID threadID);

/**
 * Set up and enqueue a VIO's work item to be processed in the base code
 * context.
 *
 * @param kvio           The VIO with the work item to be run
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, mapping to a relative priority
 **/
void enqueueKVIO(KVIO             *kvio,
                 KvdoWorkFunction  work,
                 void             *statsFunction,
                 unsigned int      action);

/**
 * Enqueue an arbitrary completion for execution on its indicated
 * thread.
 *
 * @param enqueueable  The Enqueueable object containing the completion pointer
 **/
void kvdoEnqueue(Enqueueable *enqueueable);

#endif // KERNEL_VDO_H
