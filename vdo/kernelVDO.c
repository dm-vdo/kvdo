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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.c#121 $
 */

/*
 * Sadly, this include must precede the include of kernelVDOInternals.h because
 * that file ends up including the uds version of errors.h which is wrong for
 * this file.
 */
#include "errors.h"
#include "kernelVDO.h"

#include <linux/delay.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "readOnlyNotifier.h"
#include "statistics.h"
#include "threadConfig.h"
#include "vdo.h"
#include "vdoLoad.h"
#include "vdoResize.h"
#include "vdoResizeLogical.h"
#include "vdoResume.h"
#include "vdoSuspend.h"

#include "kvio.h"

enum { PARANOID_THREAD_CONSISTENCY_CHECKS = 0 };

/**********************************************************************/
static void start_vdo_request_queue(void *ptr)
{
	struct vdo_thread *thread = ptr;
	struct vdo *vdo = thread->vdo;

	uds_register_allocating_thread(&thread->allocating_thread,
				       &vdo->allocations_allowed);
}

/**********************************************************************/
static void finish_vdo_request_queue(void *ptr)
{
	uds_unregister_allocating_thread();
}

/**********************************************************************/
static const struct vdo_work_queue_type request_queue_type = {
	.start = start_vdo_request_queue,
	.finish = finish_vdo_request_queue,
	.max_priority = VDO_REQ_Q_MAX_PRIORITY,
};

/**********************************************************************/
int make_vdo_threads(struct vdo *vdo,
		     const char *thread_name_prefix,
		     char **reason)
{
	unsigned int base_threads = vdo->thread_config->base_thread_count;
	int result = UDS_ALLOCATE(base_threads,
				  struct vdo_thread,
				  "request processing work queue",
				  &vdo->threads);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocation thread structures";
		return result;
	}

	for (vdo->initialized_thread_count = 0;
	     vdo->initialized_thread_count < base_threads;
	     vdo->initialized_thread_count++) {
		int result;
		struct vdo_thread *thread =
			&vdo->threads[vdo->initialized_thread_count];
		char queue_name[MAX_VDO_WORK_QUEUE_NAME_LEN];

		thread->vdo = vdo;
		thread->thread_id = vdo->initialized_thread_count;

		// Copy only LEN - 1 bytes and ensure NULL termination.
		vdo_get_thread_name(vdo->thread_config,
				    vdo->initialized_thread_count,
				    queue_name,
				    sizeof(queue_name));
		result = make_work_queue(thread_name_prefix,
					 queue_name,
					 &vdo->work_queue_directory,
					 vdo,
					 thread,
					 &request_queue_type,
					 1,
					 NULL,
					 &thread->request_queue);
		if (result != VDO_SUCCESS) {
			*reason = "Cannot initialize request queue";
			while (vdo->initialized_thread_count > 0) {
				unsigned int thread_to_destroy =
					vdo->initialized_thread_count - 1;
				thread = &vdo->threads[thread_to_destroy];
				finish_work_queue(thread->request_queue);
				free_work_queue(UDS_FORGET(thread->request_queue));
				vdo->initialized_thread_count--;
			}

			UDS_FREE(vdo->threads);
			vdo->threads = NULL;
			return result;
		}
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
void dump_vdo_work_queue(struct vdo *vdo)
{
	int i;

	for (i = 0; i < vdo->initialized_thread_count; i++) {
		dump_work_queue(vdo->threads[i].request_queue);
	}
}

/**********************************************************************/
void enqueue_vdo_work(struct vdo *vdo,
		      struct vdo_work_item *item,
		      thread_id_t thread_id)
{
	enqueue_work_queue(vdo->threads[thread_id].request_queue, item);
}

/**********************************************************************/
void enqueue_vio(struct vio *vio,
		 vdo_work_function work,
		 void *stats_function,
		 enum vdo_work_item_priority priority)
{
	thread_id_t thread_id = vio_as_completion(vio)->callback_thread_id;

	BUG_ON(thread_id >= vio->vdo->initialized_thread_count);
	launch_vio(vio,
		   work,
		   stats_function,
		   priority,
		   vio->vdo->threads[thread_id].request_queue);
}

/**********************************************************************/
static void vdo_enqueue_work(struct vdo_work_item *work_item)
{
	run_vdo_completion_callback(container_of(work_item,
				    struct vdo_completion,
				    work_item));
}

/**********************************************************************/
void enqueue_vdo_completion(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;
	thread_id_t thread_id = completion->callback_thread_id;

	if (ASSERT(thread_id < vdo->initialized_thread_count,
		   "thread_id %u (completion type %d) is less than thread count %u",
		   thread_id,
		   completion->type,
		   vdo->initialized_thread_count) != UDS_SUCCESS) {
		BUG();
	}

	setup_work_item(&completion->work_item, vdo_enqueue_work,
			completion->callback,
			VDO_REQ_Q_COMPLETION_PRIORITY);
	enqueue_vdo_work(vdo, &completion->work_item, thread_id);
}

/**********************************************************************/
thread_id_t vdo_get_callback_thread_id(void)
{
	struct vdo_thread *thread = get_work_queue_private_data();
	thread_id_t thread_id;

	if (thread == NULL) {
		return VDO_INVALID_THREAD_ID;
	}

	thread_id = thread->thread_id;

	if (PARANOID_THREAD_CONSISTENCY_CHECKS) {
		struct vdo *vdo = thread->vdo;

		BUG_ON(thread_id >= vdo->initialized_thread_count);
		BUG_ON(thread != &vdo->threads[thread_id]);
	}

	return thread_id;
}
