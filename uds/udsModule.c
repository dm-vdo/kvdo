/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/udsModule.c#31 $
 */

#include <linux/module.h>

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"
#include "sysfs.h"
#include "timeUtils.h"
#include "uds.h"
#include "uds-block.h"
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
EXPORT_SYMBOL_GPL(udsConfigurationGetMemory);
EXPORT_SYMBOL_GPL(udsConfigurationGetChaptersPerVolume);
EXPORT_SYMBOL_GPL(udsFreeConfiguration);
EXPORT_SYMBOL_GPL(udsGetVersion);
EXPORT_SYMBOL_GPL(udsCreateIndexSession);
EXPORT_SYMBOL_GPL(udsOpenIndex);
EXPORT_SYMBOL_GPL(udsSuspendIndexSession);
EXPORT_SYMBOL_GPL(udsResumeIndexSession);
EXPORT_SYMBOL_GPL(udsCloseIndex);
EXPORT_SYMBOL_GPL(udsDestroyIndexSession);
EXPORT_SYMBOL_GPL(udsFlushIndexSession);
EXPORT_SYMBOL_GPL(udsGetIndexConfiguration);
EXPORT_SYMBOL_GPL(udsGetIndexStats);
EXPORT_SYMBOL_GPL(udsGetIndexSessionStats);
EXPORT_SYMBOL_GPL(udsStringError);
EXPORT_SYMBOL_GPL(udsStartChunkOperation);

EXPORT_SYMBOL_GPL(allocSprintf);
EXPORT_SYMBOL_GPL(allocateMemory);
EXPORT_SYMBOL_GPL(allocateMemoryNowait);
EXPORT_SYMBOL_GPL(assertionFailed);
EXPORT_SYMBOL_GPL(assertionFailedLogOnly);
EXPORT_SYMBOL_GPL(availableSpace);
EXPORT_SYMBOL_GPL(bufferLength);
EXPORT_SYMBOL_GPL(bufferUsed);
EXPORT_SYMBOL_GPL(clearBuffer);
EXPORT_SYMBOL_GPL(compactBuffer);
EXPORT_SYMBOL_GPL(contentLength);
EXPORT_SYMBOL_GPL(copyBytes);
EXPORT_SYMBOL_GPL(currentTime);
EXPORT_SYMBOL_GPL(duplicateString);
EXPORT_SYMBOL_GPL(ensureAvailableSpace);
EXPORT_SYMBOL_GPL(equalBuffers);
EXPORT_SYMBOL_GPL(fixedSprintf);
EXPORT_SYMBOL_GPL(freeBuffer);
EXPORT_SYMBOL_GPL(freeFunnelQueue);
EXPORT_SYMBOL_GPL(freeMemory);
EXPORT_SYMBOL_GPL(funnelQueuePoll);
EXPORT_SYMBOL_GPL(getBoolean);
EXPORT_SYMBOL_GPL(getBufferContents);
EXPORT_SYMBOL_GPL(getByte);
EXPORT_SYMBOL_GPL(getBytesFromBuffer);
EXPORT_SYMBOL_GPL(getMemoryStats);
EXPORT_SYMBOL_GPL(getUInt16BEFromBuffer);
EXPORT_SYMBOL_GPL(getUInt16LEFromBuffer);
EXPORT_SYMBOL_GPL(getUInt16LEsFromBuffer);
EXPORT_SYMBOL_GPL(getUInt32BEFromBuffer);
EXPORT_SYMBOL_GPL(getUInt32BEsFromBuffer);
EXPORT_SYMBOL_GPL(getUInt32LEFromBuffer);
EXPORT_SYMBOL_GPL(getUInt64BEsFromBuffer);
EXPORT_SYMBOL_GPL(getUInt64LEFromBuffer);
EXPORT_SYMBOL_GPL(getUInt64LEsFromBuffer);
EXPORT_SYMBOL_GPL(growBuffer);
EXPORT_SYMBOL_GPL(hasSameBytes);
EXPORT_SYMBOL_GPL(isFunnelQueueEmpty);
EXPORT_SYMBOL_GPL(makeBuffer);
EXPORT_SYMBOL_GPL(makeFunnelQueue);
EXPORT_SYMBOL_GPL(MurmurHash3_x64_128);
EXPORT_SYMBOL_GPL(nowUsec);
EXPORT_SYMBOL_GPL(peekByte);
EXPORT_SYMBOL_GPL(putBoolean);
EXPORT_SYMBOL_GPL(putBuffer);
EXPORT_SYMBOL_GPL(putByte);
EXPORT_SYMBOL_GPL(putBytes);
EXPORT_SYMBOL_GPL(putInt64LEIntoBuffer);
EXPORT_SYMBOL_GPL(putUInt16BEIntoBuffer);
EXPORT_SYMBOL_GPL(putUInt16LEIntoBuffer);
EXPORT_SYMBOL_GPL(putUInt16LEsIntoBuffer);
EXPORT_SYMBOL_GPL(putUInt32BEIntoBuffer);
EXPORT_SYMBOL_GPL(putUInt32BEsIntoBuffer);
EXPORT_SYMBOL_GPL(putUInt32LEIntoBuffer);
EXPORT_SYMBOL_GPL(putUInt64BEsIntoBuffer);
EXPORT_SYMBOL_GPL(putUInt64LEIntoBuffer);
EXPORT_SYMBOL_GPL(putUInt64LEsIntoBuffer);
EXPORT_SYMBOL_GPL(reallocateMemory);
EXPORT_SYMBOL_GPL(registerAllocatingThread);
EXPORT_SYMBOL_GPL(reportMemoryUsage);
EXPORT_SYMBOL_GPL(resetBufferEnd);
EXPORT_SYMBOL_GPL(rewindBuffer);
EXPORT_SYMBOL_GPL(skipForward);
EXPORT_SYMBOL_GPL(uncompactedAmount);
EXPORT_SYMBOL_GPL(unregisterAllocatingThread);
EXPORT_SYMBOL_GPL(wrapBuffer);
EXPORT_SYMBOL_GPL(zeroBytes);

/**********************************************************************/


/**********************************************************************/

MODULE_DESCRIPTION("deduplication engine");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(UDS_VERSION);
