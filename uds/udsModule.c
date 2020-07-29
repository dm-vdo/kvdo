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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/udsModule.c#65 $
 */

#include <linux/module.h>

#include "buffer.h"
#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"
#include "sysfs.h"
#include "threadOnce.h"
#include "timeUtils.h"
#include "uds.h"
#include "util/funnelQueue.h"

/**********************************************************************/
static int __init dedupeInit(void)
{
  memory_init();
  log_info("loaded version %s", UDS_VERSION);
  init_sysfs();
  return 0;
}

/**********************************************************************/
static void __exit dedupeExit(void)
{
  put_sysfs();
  memory_exit();
  log_info("unloaded version %s", UDS_VERSION);
}

/**********************************************************************/
module_init(dedupeInit);
module_exit(dedupeExit);

EXPORT_SYMBOL_GPL(UDS_MEMORY_CONFIG_256MB);
EXPORT_SYMBOL_GPL(UDS_MEMORY_CONFIG_512MB);
EXPORT_SYMBOL_GPL(UDS_MEMORY_CONFIG_768MB);
EXPORT_SYMBOL_GPL(UDS_MEMORY_CONFIG_MAX);
EXPORT_SYMBOL_GPL(uds_initialize_configuration);
EXPORT_SYMBOL_GPL(uds_compute_index_size);
EXPORT_SYMBOL_GPL(uds_configuration_set_nonce);
EXPORT_SYMBOL_GPL(uds_configuration_get_nonce);
EXPORT_SYMBOL_GPL(uds_configuration_set_sparse);
EXPORT_SYMBOL_GPL(uds_configuration_get_sparse);
EXPORT_SYMBOL_GPL(uds_configuration_get_memory);
EXPORT_SYMBOL_GPL(uds_configuration_get_chapters_per_volume);
EXPORT_SYMBOL_GPL(uds_free_configuration);
EXPORT_SYMBOL_GPL(uds_get_version);
EXPORT_SYMBOL_GPL(uds_create_index_session);
EXPORT_SYMBOL_GPL(uds_open_index);
EXPORT_SYMBOL_GPL(uds_suspend_index_session);
EXPORT_SYMBOL_GPL(uds_resume_index_session);
EXPORT_SYMBOL_GPL(uds_close_index);
EXPORT_SYMBOL_GPL(uds_destroy_index_session);
EXPORT_SYMBOL_GPL(uds_flush_index_session);
EXPORT_SYMBOL_GPL(uds_get_index_configuration);
EXPORT_SYMBOL_GPL(uds_get_index_stats);
EXPORT_SYMBOL_GPL(uds_get_index_session_stats);
EXPORT_SYMBOL_GPL(uds_string_error);
EXPORT_SYMBOL_GPL(uds_start_chunk_operation);

EXPORT_SYMBOL_GPL(alloc_sprintf);
EXPORT_SYMBOL_GPL(allocate_memory);
EXPORT_SYMBOL_GPL(allocate_memory_nowait);
EXPORT_SYMBOL_GPL(append_to_buffer);
EXPORT_SYMBOL_GPL(assertionFailed);
EXPORT_SYMBOL_GPL(assertionFailedLogOnly);
EXPORT_SYMBOL_GPL(available_space);
EXPORT_SYMBOL_GPL(buffer_length);
EXPORT_SYMBOL_GPL(buffer_used);
EXPORT_SYMBOL_GPL(clear_buffer);
EXPORT_SYMBOL_GPL(compact_buffer);
EXPORT_SYMBOL_GPL(content_length);
EXPORT_SYMBOL_GPL(copy_bytes);
EXPORT_SYMBOL_GPL(duplicate_string);
EXPORT_SYMBOL_GPL(ensure_available_space);
EXPORT_SYMBOL_GPL(equal_buffers);
EXPORT_SYMBOL_GPL(fixed_sprintf);
EXPORT_SYMBOL_GPL(free_buffer);
EXPORT_SYMBOL_GPL(free_funnel_queue);
EXPORT_SYMBOL_GPL(free_memory);
EXPORT_SYMBOL_GPL(funnel_queue_poll);
EXPORT_SYMBOL_GPL(get_boolean);
EXPORT_SYMBOL_GPL(get_buffer_contents);
EXPORT_SYMBOL_GPL(get_byte);
EXPORT_SYMBOL_GPL(get_bytes_from_buffer);
EXPORT_SYMBOL_GPL(get_log_level);
EXPORT_SYMBOL_GPL(get_memory_stats);
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
EXPORT_SYMBOL_GPL(initialize_thread_registry);
EXPORT_SYMBOL_GPL(is_funnel_queue_empty);
EXPORT_SYMBOL_GPL(log_debug);
EXPORT_SYMBOL_GPL(log_error);
EXPORT_SYMBOL_GPL(logErrorWithStringError);
EXPORT_SYMBOL_GPL(log_info);
EXPORT_SYMBOL_GPL(log_warning);
EXPORT_SYMBOL_GPL(logWarningWithStringError);
EXPORT_SYMBOL_GPL(lookup_thread);
EXPORT_SYMBOL_GPL(make_buffer);
EXPORT_SYMBOL_GPL(make_funnel_queue);
EXPORT_SYMBOL_GPL(MurmurHash3_x64_128);
EXPORT_SYMBOL_GPL(nowUsec);
EXPORT_SYMBOL_GPL(parse_uint64);
EXPORT_SYMBOL_GPL(pause_for_logger);
EXPORT_SYMBOL_GPL(peek_byte);
EXPORT_SYMBOL_GPL(perform_once);
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
EXPORT_SYMBOL_GPL(priority_to_string);
EXPORT_SYMBOL_GPL(reallocate_memory);
EXPORT_SYMBOL_GPL(register_allocating_thread);
EXPORT_SYMBOL_GPL(registerErrorBlock);
EXPORT_SYMBOL_GPL(register_thread);
EXPORT_SYMBOL_GPL(report_memory_usage);
EXPORT_SYMBOL_GPL(reset_buffer_end);
EXPORT_SYMBOL_GPL(rewind_buffer);
EXPORT_SYMBOL_GPL(set_log_level);
EXPORT_SYMBOL_GPL(skip_forward);
EXPORT_SYMBOL_GPL(stringError);
EXPORT_SYMBOL_GPL(stringErrorName);
EXPORT_SYMBOL_GPL(string_to_priority);
EXPORT_SYMBOL_GPL(string_to_unsigned_long);
EXPORT_SYMBOL_GPL(uncompacted_amount);
EXPORT_SYMBOL_GPL(unregister_allocating_thread);
EXPORT_SYMBOL_GPL(unregister_thread);
EXPORT_SYMBOL_GPL(v_append_to_buffer);
EXPORT_SYMBOL_GPL(v_log_message);
EXPORT_SYMBOL_GPL(vLogWithStringError);
EXPORT_SYMBOL_GPL(wrap_buffer);
EXPORT_SYMBOL_GPL(zero_bytes);

/**********************************************************************/


/**********************************************************************/

MODULE_DESCRIPTION("deduplication engine");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(UDS_VERSION);
