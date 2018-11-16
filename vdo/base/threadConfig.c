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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/threadConfig.c#2 $
 */

#include "threadConfig.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "constants.h"
#include "types.h"

/**********************************************************************/
static int allocateThreadConfig(ZoneCount      logicalZoneCount,
                                ZoneCount      physicalZoneCount,
                                ZoneCount      hashZoneCount,
                                ZoneCount      baseThreadCount,
                                ThreadConfig **configPtr)
{
  ThreadConfig *config;
  int result = ALLOCATE(1, ThreadConfig, "thread config", &config);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(logicalZoneCount, ThreadID, "logical thread array",
                    &config->logicalThreads);
  if (result != VDO_SUCCESS) {
    freeThreadConfig(&config);
    return result;
  }

  result = ALLOCATE(physicalZoneCount, ThreadID, "physical thread array",
                    &config->physicalThreads);
  if (result != VDO_SUCCESS) {
    freeThreadConfig(&config);
    return result;
  }

  result = ALLOCATE(hashZoneCount, ThreadID, "hash thread array",
                    &config->hashZoneThreads);
  if (result != VDO_SUCCESS) {
    freeThreadConfig(&config);
    return result;
  }

  config->logicalZoneCount  = logicalZoneCount;
  config->physicalZoneCount = physicalZoneCount;
  config->hashZoneCount     = hashZoneCount;
  config->baseThreadCount   = baseThreadCount;

  *configPtr = config;
  return VDO_SUCCESS;
}

/**********************************************************************/
static void assignThreadIDs(ThreadID   threadIDs[],
                            ZoneCount  count,
                            ThreadID  *idPtr)
{
  for (ZoneCount zone = 0; zone < count; zone++) {
    threadIDs[zone] = (*idPtr)++;
  }
}

/**********************************************************************/
int makeThreadConfig(ZoneCount      logicalZoneCount,
                     ZoneCount      physicalZoneCount,
                     ZoneCount      hashZoneCount,
                     ThreadConfig **configPtr)
{
  if ((logicalZoneCount == 0)
      && (physicalZoneCount == 0)
      && (hashZoneCount == 0)) {
    return makeOneThreadConfig(configPtr);
  }

  if (physicalZoneCount > MAX_PHYSICAL_ZONES) {
    return logErrorWithStringError(VDO_BAD_CONFIGURATION,
                                   "Physical zone count %u exceeds maximum "
                                   "(%u)",
                                   physicalZoneCount, MAX_PHYSICAL_ZONES);
  }

  if (logicalZoneCount > MAX_LOGICAL_ZONES) {
    return logErrorWithStringError(VDO_BAD_CONFIGURATION,
                                   "Logical zone count %u exceeds maximum "
                                   "(%u)",
                                   logicalZoneCount, MAX_LOGICAL_ZONES);
  }

  ThreadConfig *config;
  ThreadCount total = logicalZoneCount + physicalZoneCount + hashZoneCount + 2;
  int result = allocateThreadConfig(logicalZoneCount, physicalZoneCount,
                                    hashZoneCount, total, &config);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ThreadID id = 0;
  config->adminThread   = id;
  config->journalThread = id++;
  config->packerThread  = id++;
  assignThreadIDs(config->logicalThreads, logicalZoneCount, &id);
  assignThreadIDs(config->physicalThreads, physicalZoneCount, &id);
  assignThreadIDs(config->hashZoneThreads, hashZoneCount, &id);

  ASSERT_LOG_ONLY(id == total, "correct number of thread IDs assigned");

  *configPtr = config;
  return VDO_SUCCESS;
}

/**********************************************************************/
int makeZeroThreadConfig(ThreadConfig **configPtr)
{
  ThreadConfig *config;
  int result = ALLOCATE(1, ThreadConfig, __func__, &config);
  if (result != VDO_SUCCESS) {
    return result;
  }

  config->logicalZoneCount  = 0;
  config->physicalZoneCount = 0;
  config->hashZoneCount     = 0;
  config->baseThreadCount   = 0;
  *configPtr                = config;
  return VDO_SUCCESS;
}

/**********************************************************************/
int makeOneThreadConfig(ThreadConfig **configPtr)
{
  ThreadConfig *config;
  int result = allocateThreadConfig(1, 1, 1, 1, &config);
  if (result != VDO_SUCCESS) {
    return result;
  }

  config->logicalThreads[0]  = 0;
  config->physicalThreads[0] = 0;
  config->hashZoneThreads[0] = 0;
  *configPtr = config;
  return VDO_SUCCESS;
}

/**********************************************************************/
int copyThreadConfig(const ThreadConfig *oldConfig, ThreadConfig **configPtr)
{
  ThreadConfig *config;
  int result = allocateThreadConfig(oldConfig->logicalZoneCount,
                                    oldConfig->physicalZoneCount,
                                    oldConfig->hashZoneCount,
                                    oldConfig->baseThreadCount,
                                    &config);
  if (result != VDO_SUCCESS) {
    return result;
  }

  config->adminThread   = oldConfig->adminThread;
  config->journalThread = oldConfig->journalThread;
  config->packerThread  = oldConfig->packerThread;
  for (ZoneCount i = 0; i < config->logicalZoneCount; i++) {
    config->logicalThreads[i] = oldConfig->logicalThreads[i];
  }
  for (ZoneCount i = 0; i < config->physicalZoneCount; i++) {
    config->physicalThreads[i] = oldConfig->physicalThreads[i];
  }
  for (ZoneCount i = 0; i < config->hashZoneCount; i++) {
    config->hashZoneThreads[i] = oldConfig->hashZoneThreads[i];
  }

  *configPtr = config;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeThreadConfig(ThreadConfig **configPtr)
{
  if (*configPtr == NULL) {
    return;
  }

  ThreadConfig *config = *configPtr;
  *configPtr           = NULL;

  FREE(config->logicalThreads);
  FREE(config->physicalThreads);
  FREE(config->hashZoneThreads);
  FREE(config);
}

/**********************************************************************/
static bool getZoneThreadName(const ThreadID  threadIDs[],
                              ZoneCount       count,
                              ThreadID        id,
                              const char     *prefix,
                              char           *buffer,
                              size_t          bufferLength)
{
  if (id >= threadIDs[0]) {
    ThreadID index = id - threadIDs[0];
    if (index < count) {
      snprintf(buffer, bufferLength, "%s%d", prefix, index);
      return true;
    }
  }
  return false;
}

/**********************************************************************/
void getVDOThreadName(const ThreadConfig *threadConfig,
                      ThreadID            threadID,
                      char               *buffer,
                      size_t              bufferLength)
{
  if (threadConfig->baseThreadCount == 1) {
    // Historically this was the "request queue" thread.
    snprintf(buffer, bufferLength, "reqQ");
    return;
  }
  if (threadID == threadConfig->journalThread) {
    snprintf(buffer, bufferLength, "journalQ");
    return;
  } else if (threadID == threadConfig->adminThread) {
    // Theoretically this could be different from the journal thread.
    snprintf(buffer, bufferLength, "adminQ");
    return;
  } else if (threadID == threadConfig->packerThread) {
    snprintf(buffer, bufferLength, "packerQ");
    return;
  }
  if (getZoneThreadName(threadConfig->logicalThreads,
                        threadConfig->logicalZoneCount,
                        threadID, "logQ", buffer, bufferLength)) {
    return;
  }
  if (getZoneThreadName(threadConfig->physicalThreads,
                        threadConfig->physicalZoneCount,
                        threadID, "physQ", buffer, bufferLength)) {
    return;
  }
  if (getZoneThreadName(threadConfig->hashZoneThreads,
                        threadConfig->hashZoneCount,
                        threadID, "hashQ", buffer, bufferLength)) {
    return;
  }

  // Some sort of misconfiguration?
  snprintf(buffer, bufferLength, "reqQ%d", threadID);
}
