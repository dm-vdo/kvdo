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
 * $Id: //eng/uds-releases/gloria/kernelLinux/uds/udsModule.c#6 $
 */

#include <linux/module.h>

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "sysfs.h"
#include "timeUtils.h"
#include "uds.h"
#include "uds-block.h"
#include "uds-param.h"
#include "util/funnelQueue.h"

/**********************************************************************/
static int __init dedupeInit(void)
{
  memoryInit();
  logInfo("loaded version %s", UDS_VERSION);
  initSysfs();
  return 0;
}

/**********************************************************************/
static void __exit dedupeExit(void)
{
  udsShutdown();
  putSysfs();
  memoryExit();
  logInfo("unloaded version %s", UDS_VERSION);
}

/**********************************************************************/
module_init(dedupeInit);
module_exit(dedupeExit);

EXPORT_SYMBOL_GPL(UDS_MEMORY_CONFIG_256MB);
EXPORT_SYMBOL_GPL(UDS_MEMORY_CONFIG_512MB);
EXPORT_SYMBOL_GPL(UDS_MEMORY_CONFIG_768MB);
EXPORT_SYMBOL_GPL(UDS_MEMORY_CONFIG_MAX);
EXPORT_SYMBOL_GPL(udsInitializeConfiguration);
EXPORT_SYMBOL_GPL(udsComputeIndexSize);
EXPORT_SYMBOL_GPL(udsConfigurationSetNonce);
EXPORT_SYMBOL_GPL(udsConfigurationGetNonce);
EXPORT_SYMBOL_GPL(udsConfigurationSetSparse);
EXPORT_SYMBOL_GPL(udsConfigurationGetSparse);
EXPORT_SYMBOL_GPL(udsConfigurationSetCheckpointFrequency);
EXPORT_SYMBOL_GPL(udsConfigurationGetCheckpointFrequency);
EXPORT_SYMBOL_GPL(udsConfigurationGetMemory);
EXPORT_SYMBOL_GPL(udsConfigurationGetChaptersPerVolume);
EXPORT_SYMBOL_GPL(udsFreeConfiguration);
EXPORT_SYMBOL_GPL(udsGetVersion);
EXPORT_SYMBOL_GPL(udsCreateLocalIndex);
EXPORT_SYMBOL_GPL(udsLoadLocalIndex);
EXPORT_SYMBOL_GPL(udsRebuildLocalIndex);
EXPORT_SYMBOL_GPL(udsCloseIndexSession);
EXPORT_SYMBOL_GPL(udsGetIndexConfiguration);
EXPORT_SYMBOL_GPL(udsGetIndexStats);
EXPORT_SYMBOL_GPL(udsStringError);

EXPORT_SYMBOL_GPL(udsOpenBlockContext);
EXPORT_SYMBOL_GPL(udsCloseBlockContext);
EXPORT_SYMBOL_GPL(udsFlushBlockContext);
EXPORT_SYMBOL_GPL(udsStartChunkOperation);
EXPORT_SYMBOL_GPL(udsGetBlockContextConfiguration);
EXPORT_SYMBOL_GPL(udsGetBlockContextIndexStats);
EXPORT_SYMBOL_GPL(udsGetBlockContextStats);

EXPORT_SYMBOL_GPL(UDS_PARAM_FALSE);
EXPORT_SYMBOL_GPL(UDS_PARAM_TRUE);
EXPORT_SYMBOL_GPL(udsGetParameter);
EXPORT_SYMBOL_GPL(udsIterateParameter);
EXPORT_SYMBOL_GPL(udsResetParameter);
EXPORT_SYMBOL_GPL(udsSetParameter);
EXPORT_SYMBOL_GPL(udsStringValue);
EXPORT_SYMBOL_GPL(udsUnsignedValue);

EXPORT_SYMBOL_GPL(allocSprintf);
EXPORT_SYMBOL_GPL(allocateMemory);
EXPORT_SYMBOL_GPL(assertionFailed);
EXPORT_SYMBOL_GPL(assertionFailedLogOnly);
EXPORT_SYMBOL_GPL(compactBuffer);
EXPORT_SYMBOL_GPL(contentLength);
EXPORT_SYMBOL_GPL(copyBytes);
EXPORT_SYMBOL_GPL(currentTime);
EXPORT_SYMBOL_GPL(duplicateString);
EXPORT_SYMBOL_GPL(ensureAvailableSpace);
EXPORT_SYMBOL_GPL(fixedSprintf);
EXPORT_SYMBOL_GPL(freeBuffer);
EXPORT_SYMBOL_GPL(freeFunnelQueue);
EXPORT_SYMBOL_GPL(freeMemory);
EXPORT_SYMBOL_GPL(funnelQueuePoll);
EXPORT_SYMBOL_GPL(getBytesFromBuffer);
EXPORT_SYMBOL_GPL(getMemoryStats);
EXPORT_SYMBOL_GPL(isFunnelQueueEmpty);
EXPORT_SYMBOL_GPL(makeBuffer);
EXPORT_SYMBOL_GPL(makeFunnelQueue);
EXPORT_SYMBOL_GPL(nowUsec);
EXPORT_SYMBOL_GPL(putBytes);
EXPORT_SYMBOL_GPL(reallocateMemory);
EXPORT_SYMBOL_GPL(recordBioAlloc);
EXPORT_SYMBOL_GPL(recordBioFree);
EXPORT_SYMBOL_GPL(registerAllocatingThread);
EXPORT_SYMBOL_GPL(reportMemoryUsage);
EXPORT_SYMBOL_GPL(rewindBuffer);
EXPORT_SYMBOL_GPL(skipForward);
EXPORT_SYMBOL_GPL(unregisterAllocatingThread);

/**********************************************************************/


/**********************************************************************/

MODULE_DESCRIPTION("deduplication engine");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(UDS_VERSION);
