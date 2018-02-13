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
 * $Id: //eng/uds-releases/flanders/kernelLinux/uds/udsModule.c#9 $
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

EXPORT_SYMBOL(UDS_MEMORY_CONFIG_256MB);
EXPORT_SYMBOL(UDS_MEMORY_CONFIG_512MB);
EXPORT_SYMBOL(UDS_MEMORY_CONFIG_768MB);
EXPORT_SYMBOL(UDS_MEMORY_CONFIG_MAX);
EXPORT_SYMBOL(udsCalculateSHA256ChunkName);
EXPORT_SYMBOL(udsCalculateMurmur3ChunkName);
EXPORT_SYMBOL(udsEqualChunkName);
EXPORT_SYMBOL(udsInitializeConfiguration);
EXPORT_SYMBOL(udsComputeIndexSize);
EXPORT_SYMBOL(udsConfigurationSetNonce);
EXPORT_SYMBOL(udsConfigurationGetNonce);
EXPORT_SYMBOL(udsConfigurationSetSparse);
EXPORT_SYMBOL(udsConfigurationGetSparse);
EXPORT_SYMBOL(udsConfigurationSetCheckpointFrequency);
EXPORT_SYMBOL(udsConfigurationGetCheckpointFrequency);
EXPORT_SYMBOL(udsConfigurationGetMemory);
EXPORT_SYMBOL(udsConfigurationGetChaptersPerVolume);
EXPORT_SYMBOL(udsFreeConfiguration);
EXPORT_SYMBOL(udsGetVersion);
EXPORT_SYMBOL(udsCreateLocalIndex);
EXPORT_SYMBOL(udsLoadLocalIndex);
EXPORT_SYMBOL(udsRebuildLocalIndex);
EXPORT_SYMBOL(udsCloseIndexSession);
EXPORT_SYMBOL(udsStringError);

EXPORT_SYMBOL(udsOpenBlockContext);
EXPORT_SYMBOL(udsCloseBlockContext);
EXPORT_SYMBOL(udsFlushBlockContext);
EXPORT_SYMBOL(udsRegisterDedupeBlockCallback);
EXPORT_SYMBOL(udsSetBlockContextRequestQueueLimit);
EXPORT_SYMBOL(udsSetBlockContextHashAlgorithm);
EXPORT_SYMBOL(udsPostBlock);
EXPORT_SYMBOL(udsPostBlockName);
EXPORT_SYMBOL(udsQueryBlockName);
EXPORT_SYMBOL(udsCheckBlock);
EXPORT_SYMBOL(udsUpdateBlockMapping);
EXPORT_SYMBOL(udsDeleteBlockMapping);
EXPORT_SYMBOL(udsStartChunkOperation);
EXPORT_SYMBOL(udsGetBlockContextConfiguration);
EXPORT_SYMBOL(udsGetBlockContextIndexStats);
EXPORT_SYMBOL(udsGetBlockContextStats);
EXPORT_SYMBOL(udsResetBlockContextStats);

EXPORT_SYMBOL(UDS_PARAM_FALSE);
EXPORT_SYMBOL(UDS_PARAM_TRUE);
EXPORT_SYMBOL(udsGetParameter);
EXPORT_SYMBOL(udsIterateParameter);
EXPORT_SYMBOL(udsResetParameter);
EXPORT_SYMBOL(udsSetParameter);
EXPORT_SYMBOL(udsStringValue);
EXPORT_SYMBOL(udsUnsignedValue);

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
EXPORT_SYMBOL_GPL(freeMemory);
EXPORT_SYMBOL_GPL(getBytesFromBuffer);
EXPORT_SYMBOL_GPL(getMemoryStats);
EXPORT_SYMBOL_GPL(makeBuffer);
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
