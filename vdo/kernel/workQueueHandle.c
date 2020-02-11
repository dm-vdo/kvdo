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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueueHandle.c#8 $
 */

#include "workQueueHandle.h"

struct work_queue_stack_handle_globals work_queue_stack_handle_globals;

/**********************************************************************/
void initialize_work_queue_stack_handle(struct work_queue_stack_handle *handle,
					struct simple_work_queue *queue)
{
	handle->nonce = work_queue_stack_handle_globals.nonce;
	handle->queue = queue;

	long offset = (char *)handle - (char *)task_stack_page(current);

	spin_lock(&work_queue_stack_handle_globals.offset_lock);
	if (work_queue_stack_handle_globals.offset == 0) {
		work_queue_stack_handle_globals.offset = offset;
		spin_unlock(&work_queue_stack_handle_globals.offset_lock);
	} else {
		long found_offset = work_queue_stack_handle_globals.offset;

		spin_unlock(&work_queue_stack_handle_globals.offset_lock);
		BUG_ON(found_offset != offset);
	}
}

/**********************************************************************/
void init_work_queue_stack_handle_once(void)
{
	spin_lock_init(&work_queue_stack_handle_globals.offset_lock);
	work_queue_stack_handle_globals.nonce = ktime_get_ns();
}
