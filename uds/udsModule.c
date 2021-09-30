/*
 * Copyright Red Hat
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
 * $Id: //eng/uds-releases/lisa/kernelLinux/uds/udsModule.c#14 $
 */

#include <linux/module.h>

#include "buffer.h"
#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "sysfs.h"
#include "threadDevice.h"
#include "threadOnce.h"
#include "timeUtils.h"
#include "uds.h"
#include "util/funnelQueue.h"

/**********************************************************************/
static int __init dedupe_init(void)
{
	uds_initialize_thread_device_registry();
	uds_memory_init();
	uds_log_info("loaded version %s", UDS_VERSION);
	init_uds_sysfs();
	return 0;
}

/**********************************************************************/
static void __exit dedupe_exit(void)
{
	put_uds_sysfs();
	uds_memory_exit();
	uds_log_info("unloaded version %s", UDS_VERSION);
}

/**********************************************************************/
module_init(dedupe_init);
module_exit(dedupe_exit);

EXPORT_SYMBOL_GPL(uds_close_index);
EXPORT_SYMBOL_GPL(uds_compute_index_size);
EXPORT_SYMBOL_GPL(uds_create_index_session);
EXPORT_SYMBOL_GPL(uds_destroy_index_session);
EXPORT_SYMBOL_GPL(uds_flush_index_session);
EXPORT_SYMBOL_GPL(uds_get_index_parameters);
EXPORT_SYMBOL_GPL(uds_get_index_stats);
EXPORT_SYMBOL_GPL(uds_get_version);
EXPORT_SYMBOL_GPL(uds_open_index);
EXPORT_SYMBOL_GPL(uds_resume_index_session);
EXPORT_SYMBOL_GPL(uds_start_chunk_operation);
EXPORT_SYMBOL_GPL(uds_suspend_index_session);

EXPORT_SYMBOL_GPL(__uds_log_message);
EXPORT_SYMBOL_GPL(__uds_log_strerror);
EXPORT_SYMBOL_GPL(available_space);
EXPORT_SYMBOL_GPL(buffer_length);
EXPORT_SYMBOL_GPL(buffer_used);
EXPORT_SYMBOL_GPL(clear_buffer);
EXPORT_SYMBOL_GPL(compact_buffer);
EXPORT_SYMBOL_GPL(content_length);
EXPORT_SYMBOL_GPL(copy_bytes);
EXPORT_SYMBOL_GPL(current_time_us);
EXPORT_SYMBOL_GPL(ensure_available_space);
EXPORT_SYMBOL_GPL(equal_buffers);
EXPORT_SYMBOL_GPL(free_buffer);
EXPORT_SYMBOL_GPL(free_funnel_queue);
EXPORT_SYMBOL_GPL(funnel_queue_poll);
EXPORT_SYMBOL_GPL(get_boolean);
EXPORT_SYMBOL_GPL(get_buffer_contents);
EXPORT_SYMBOL_GPL(get_byte);
EXPORT_SYMBOL_GPL(get_bytes_from_buffer);
EXPORT_SYMBOL_GPL(get_uds_log_level);
EXPORT_SYMBOL_GPL(get_uds_memory_stats);
EXPORT_SYMBOL_GPL(get_uint16_le_from_buffer);
EXPORT_SYMBOL_GPL(get_uint16_les_from_buffer);
EXPORT_SYMBOL_GPL(get_uint32_le_from_buffer);
EXPORT_SYMBOL_GPL(get_uint64_le_from_buffer);
EXPORT_SYMBOL_GPL(get_uint64_les_from_buffer);
EXPORT_SYMBOL_GPL(has_same_bytes);
EXPORT_SYMBOL_GPL(is_funnel_queue_empty);
EXPORT_SYMBOL_GPL(make_buffer);
EXPORT_SYMBOL_GPL(make_funnel_queue);
EXPORT_SYMBOL_GPL(perform_once);
EXPORT_SYMBOL_GPL(put_boolean);
EXPORT_SYMBOL_GPL(put_buffer);
EXPORT_SYMBOL_GPL(put_byte);
EXPORT_SYMBOL_GPL(put_bytes);
EXPORT_SYMBOL_GPL(put_int64_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint16_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint16_les_into_buffer);
EXPORT_SYMBOL_GPL(put_uint32_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint64_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint64_les_into_buffer);
EXPORT_SYMBOL_GPL(register_error_block);
EXPORT_SYMBOL_GPL(report_uds_memory_usage);
EXPORT_SYMBOL_GPL(reset_buffer_end);
EXPORT_SYMBOL_GPL(rewind_buffer);
EXPORT_SYMBOL_GPL(set_uds_log_level);
EXPORT_SYMBOL_GPL(skip_forward);
EXPORT_SYMBOL_GPL(uds_alloc_sprintf);
EXPORT_SYMBOL_GPL(uds_allocate_memory);
EXPORT_SYMBOL_GPL(uds_allocate_memory_nowait);
EXPORT_SYMBOL_GPL(uds_append_to_buffer);
EXPORT_SYMBOL_GPL(uds_assertion_failed);
EXPORT_SYMBOL_GPL(uds_duplicate_string);
EXPORT_SYMBOL_GPL(uds_fixed_sprintf);
EXPORT_SYMBOL_GPL(uds_free_memory);
EXPORT_SYMBOL_GPL(uds_get_thread_device_id);
EXPORT_SYMBOL_GPL(uds_initialize_thread_registry);
EXPORT_SYMBOL_GPL(uds_log_backtrace);
EXPORT_SYMBOL_GPL(uds_log_priority_to_string);
EXPORT_SYMBOL_GPL(uds_log_string_to_priority);
EXPORT_SYMBOL_GPL(uds_lookup_thread);
EXPORT_SYMBOL_GPL(uds_parse_uint64);
EXPORT_SYMBOL_GPL(uds_pause_for_logger);
EXPORT_SYMBOL_GPL(uds_reallocate_memory);
EXPORT_SYMBOL_GPL(uds_register_allocating_thread);
EXPORT_SYMBOL_GPL(uds_register_thread);
EXPORT_SYMBOL_GPL(uds_register_thread_device_id);
EXPORT_SYMBOL_GPL(uds_string_error);
EXPORT_SYMBOL_GPL(uds_string_error_name);
EXPORT_SYMBOL_GPL(uds_string_to_unsigned_long);
EXPORT_SYMBOL_GPL(uds_unregister_allocating_thread);
EXPORT_SYMBOL_GPL(uds_unregister_thread);
EXPORT_SYMBOL_GPL(uds_unregister_thread_device_id);
EXPORT_SYMBOL_GPL(uds_v_append_to_buffer);
EXPORT_SYMBOL_GPL(uds_vlog_strerror);
EXPORT_SYMBOL_GPL(uncompacted_amount);
EXPORT_SYMBOL_GPL(wrap_buffer);
EXPORT_SYMBOL_GPL(zero_bytes);

/**********************************************************************/


/**********************************************************************/

MODULE_DESCRIPTION("deduplication engine");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(UDS_VERSION);
