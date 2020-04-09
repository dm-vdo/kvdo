/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/udsModule.c#6 $
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
  init_sysfs();
  return 0;
}

/**********************************************************************/
static void __exit dedupeExit(void)
{
  put_sysfs();
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
EXPORT_SYMBOL_GPL(uds_compute_index_size);
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
EXPORT_SYMBOL_GPL(available_space);
EXPORT_SYMBOL_GPL(buffer_length);
EXPORT_SYMBOL_GPL(buffer_used);
EXPORT_SYMBOL_GPL(clear_buffer);
EXPORT_SYMBOL_GPL(compact_buffer);
EXPORT_SYMBOL_GPL(content_length);
EXPORT_SYMBOL_GPL(copy_bytes);
EXPORT_SYMBOL_GPL(currentTime);
EXPORT_SYMBOL_GPL(duplicateString);
EXPORT_SYMBOL_GPL(ensure_available_space);
EXPORT_SYMBOL_GPL(equal_buffers);
EXPORT_SYMBOL_GPL(fixedSprintf);
EXPORT_SYMBOL_GPL(free_buffer);
EXPORT_SYMBOL_GPL(freeFunnelQueue);
EXPORT_SYMBOL_GPL(freeMemory);
EXPORT_SYMBOL_GPL(funnelQueuePoll);
EXPORT_SYMBOL_GPL(get_boolean);
EXPORT_SYMBOL_GPL(get_buffer_contents);
EXPORT_SYMBOL_GPL(get_byte);
EXPORT_SYMBOL_GPL(get_bytes_from_buffer);
EXPORT_SYMBOL_GPL(getMemoryStats);
EXPORT_SYMBOL_GPL(get_uint16_be_from_buffer);
EXPORT_SYMBOL_GPL(get_uint16_le_from_buffer);
EXPORT_SYMBOL_GPL(get_uint16_les_from_buffer);
EXPORT_SYMBOL_GPL(get_uint32_be_from_buffer);
EXPORT_SYMBOL_GPL(get_uint32_bes_from_buffer);
EXPORT_SYMBOL_GPL(get_uint32_le_from_buffer);
EXPORT_SYMBOL_GPL(get_uint64_bes_from_buffer);
EXPORT_SYMBOL_GPL(get_uint64_le_from_buffer);
EXPORT_SYMBOL_GPL(get_uint64_les_from_buffer);
EXPORT_SYMBOL_GPL(grow_buffer);
EXPORT_SYMBOL_GPL(has_same_bytes);
EXPORT_SYMBOL_GPL(isFunnelQueueEmpty);
EXPORT_SYMBOL_GPL(make_buffer);
EXPORT_SYMBOL_GPL(makeFunnelQueue);
EXPORT_SYMBOL_GPL(MurmurHash3_x64_128);
EXPORT_SYMBOL_GPL(nowUsec);
EXPORT_SYMBOL_GPL(peek_byte);
EXPORT_SYMBOL_GPL(put_boolean);
EXPORT_SYMBOL_GPL(put_buffer);
EXPORT_SYMBOL_GPL(put_byte);
EXPORT_SYMBOL_GPL(put_bytes);
EXPORT_SYMBOL_GPL(put_int64_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint16_be_into_buffer);
EXPORT_SYMBOL_GPL(put_uint16_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint16_les_into_buffer);
EXPORT_SYMBOL_GPL(put_uint32_be_into_buffer);
EXPORT_SYMBOL_GPL(put_uint32_bes_into_buffer);
EXPORT_SYMBOL_GPL(put_uint32_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint64_bes_into_buffer);
EXPORT_SYMBOL_GPL(put_uint64_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint64_les_into_buffer);
EXPORT_SYMBOL_GPL(reallocateMemory);
EXPORT_SYMBOL_GPL(registerAllocatingThread);
EXPORT_SYMBOL_GPL(reportMemoryUsage);
EXPORT_SYMBOL_GPL(reset_buffer_end);
EXPORT_SYMBOL_GPL(rewind_buffer);
EXPORT_SYMBOL_GPL(skip_forward);
EXPORT_SYMBOL_GPL(uncompacted_amount);
EXPORT_SYMBOL_GPL(unregisterAllocatingThread);
EXPORT_SYMBOL_GPL(wrap_buffer);
EXPORT_SYMBOL_GPL(zero_bytes);

/**********************************************************************/


/**********************************************************************/

MODULE_DESCRIPTION("deduplication engine");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(UDS_VERSION);
