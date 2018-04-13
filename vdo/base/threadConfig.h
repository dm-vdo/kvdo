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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/threadConfig.h#1 $
 */

#ifndef THREAD_CONFIG_H
#define THREAD_CONFIG_H

#include "permassert.h"

#include "types.h"

struct threadConfig {
  ZoneCount    logicalZoneCount;
  ZoneCount    physicalZoneCount;
  ZoneCount    hashZoneCount;
  ThreadCount  baseThreadCount;
  ThreadID     adminThread;
  ThreadID     journalThread;
  ThreadID     packerThread;
  ThreadID    *logicalThreads;
  ThreadID    *physicalThreads;
  ThreadID    *hashZoneThreads;
};

/**
 * Make a thread configuration. If both the logical zone count and the
 * physical zone count are set to 0, a one thread configuration will be
 * made.
 *
 * @param [in]  logicalZoneCount    The number of logical zones
 * @param [in]  physicalZoneCount   The number of physical zones
 * @param [in]  hashZoneCount       The number of hash zones
 * @param [out] configPtr           A pointer to hold the new thread
 *                                  configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int makeThreadConfig(ZoneCount      logicalZoneCount,
                     ZoneCount      physicalZoneCount,
                     ZoneCount      hashZoneCount,
                     ThreadConfig **configPtr)
  __attribute__((warn_unused_result));

/**
 * Make a thread configuration that uses no threads. This is the configuration
 * for VDOs which are constructed from user mode that have only a synchronous
 * layer.
 *
 * @param [out] configPtr   A pointer to hold the new thread configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int makeZeroThreadConfig(ThreadConfig **configPtr);

/**
 * Make a thread configuration that uses only one thread.
 *
 * @param [out] configPtr      A pointer to hold the new thread configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int makeOneThreadConfig(ThreadConfig **configPtr)
  __attribute__((warn_unused_result));

/**
 * Make a new thread config which is a copy of an existing one.
 *
 * @param [in]  oldConfig       The thread configuration to copy
 * @param [out] configPtr       A pointer to hold the new thread configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int copyThreadConfig(const ThreadConfig *oldConfig, ThreadConfig **configPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy a thread configuration and null out the reference to it.
 *
 * @param configPtr  The reference to the thread configuration to destroy
 **/
void freeThreadConfig(ThreadConfig **configPtr);

/**
 * Get the thread id for a given logical zone.
 *
 * @param threadConfig  the thread config
 * @param logicalZone   the number of the logical zone
 *
 * @return the thread id for the given zone
 **/
__attribute__((warn_unused_result))
static inline ThreadID getLogicalZoneThread(const ThreadConfig *threadConfig,
                                            ZoneCount           logicalZone)
{
  ASSERT_LOG_ONLY((logicalZone <= threadConfig->logicalZoneCount),
                  "logical zone valid");
  return threadConfig->logicalThreads[logicalZone];
}

/**
 * Get the thread id for a given physical zone.
 *
 * @param threadConfig  the thread config
 * @param physicalZone  the number of the physical zone
 *
 * @return the thread id for the given zone
 **/
__attribute__((warn_unused_result))
static inline ThreadID getPhysicalZoneThread(const ThreadConfig *threadConfig,
                                             ZoneCount           physicalZone)
{
  ASSERT_LOG_ONLY((physicalZone <= threadConfig->physicalZoneCount),
                  "physical zone valid");
  return threadConfig->physicalThreads[physicalZone];
}

/**
 * Get the thread id for a given hash zone.
 *
 * @param threadConfig  the thread config
 * @param hashZone      the number of the hash zone
 *
 * @return the thread id for the given zone
 **/
__attribute__((warn_unused_result))
static inline ThreadID getHashZoneThread(const ThreadConfig *threadConfig,
                                         ZoneCount           hashZone)
{
  ASSERT_LOG_ONLY((hashZone <= threadConfig->hashZoneCount),
                  "hash zone valid");
  return threadConfig->hashZoneThreads[hashZone];
}

/**
 * Get the thread id for the journal zone.
 *
 * @param threadConfig  the thread config
 *
 * @return the thread id for the journal zone
 **/
__attribute__((warn_unused_result))
static inline ThreadID getJournalZoneThread(const ThreadConfig *threadConfig)
{
  return threadConfig->journalThread;
}

/**
 * Get the thread id for the packer zone.
 *
 * @param threadConfig  the thread config
 *
 * @return the thread id for the packer zone
 **/
__attribute__((warn_unused_result))
static inline ThreadID getPackerZoneThread(const ThreadConfig *threadConfig)
{
  return threadConfig->packerThread;
}

/**
 * Get the thread ID for admin requests.
 *
 * @param threadConfig  The thread config
 *
 * @return the thread id to use for admin requests
 **/
__attribute__((warn_unused_result))
static inline ThreadID getAdminThread(const ThreadConfig *threadConfig)
{
  return threadConfig->adminThread;
}

/**
 * Format the name of the worker thread desired to support a given
 * work queue. The physical layer may add a prefix identifying the
 * product; the output from this function should just identify the
 * thread.
 *
 * @param threadConfig  The thread configuration
 * @param threadID      The thread id
 * @param buffer        Where to put the formatted name
 * @param bufferLength  Size of the output buffer
 **/
void getVDOThreadName(const ThreadConfig *threadConfig,
                      ThreadID            threadID,
                      char               *buffer,
                      size_t              bufferLength);

#endif /* THREAD_CONFIG_H */
