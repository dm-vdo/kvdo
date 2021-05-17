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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.c#98 $
 */

/*
 * Sadly, this include must precede the include of kernelVDOInternals.h because
 * that file ends up including the uds version of errors.h which is wrong for
 * this file.
 */
#include "errors.h"
#include "kernelVDOInternals.h"

#include <linux/delay.h>

#include "memoryAlloc.h"
#include "permassert.h"

#include "physicalLayer.h"
#include "readOnlyNotifier.h"
#include "statistics.h"
#include "threadConfig.h"
#include "vdo.h"
#include "vdoLoad.h"
#include "vdoResize.h"
#include "vdoResizeLogical.h"
#include "vdoResume.h"
#include "vdoSuspend.h"

#include "kernelLayer.h"
#include "kvio.h"
#include "logger.h"

enum { PARANOID_THREAD_CONSISTENCY_CHECKS = 0 };

/**********************************************************************/
static void start_vdo_request_queue(void *ptr)
{
	struct vdo_thread *thread = ptr;
	struct vdo *vdo = thread->vdo;
	register_allocating_thread(&thread->allocating_thread,
				   &vdo->allocations_allowed);
}

/**********************************************************************/
static void finish_vdo_request_queue(void *ptr)
{
	unregister_allocating_thread();
}

/**********************************************************************/
static const struct vdo_work_queue_type request_queue_type = {
	.start = start_vdo_request_queue,
	.finish = finish_vdo_request_queue,
	.action_table = {

			{ .name = "req_completion",
			  .code = REQ_Q_ACTION_COMPLETION,
			  .priority = 1 },
			{ .name = "req_flush",
			  .code = REQ_Q_ACTION_FLUSH,
			  .priority = 2 },
			{ .name = "req_map_bio",
			  .code = REQ_Q_ACTION_MAP_BIO,
			  .priority = 0 },
			{ .name = "req_sync",
			  .code = REQ_Q_ACTION_SYNC,
			  .priority = 2 },
			{ .name = "req_vio_callback",
			  .code = REQ_Q_ACTION_VIO_CALLBACK,
			  .priority = 1 },
		},
};

/**********************************************************************/
int make_vdo_threads(struct vdo *vdo, char **reason)
{
	struct kernel_layer *layer = vdo_as_kernel_layer(vdo);
	unsigned int base_threads = vdo->thread_config->base_thread_count;
	int result = ALLOCATE(base_threads,
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
		char queue_name[MAX_QUEUE_NAME_LEN];

		thread->vdo = vdo;
		thread->thread_id = vdo->initialized_thread_count;

		// Copy only LEN - 1 bytes and ensure NULL termination.
		get_vdo_thread_name(vdo->thread_config,
				    vdo->initialized_thread_count,
				    queue_name,
				    sizeof(queue_name));
		result = make_work_queue(layer->thread_name_prefix,
					 queue_name,
					 &layer->vdo.work_queue_directory,
					 layer,
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
				free_work_queue(&thread->request_queue);
				vdo->initialized_thread_count--;
			}

			FREE(vdo->threads);
			vdo->threads = NULL;
			return result;
		}
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int start_vdo(struct vdo *vdo, char **reason)
{
	int result = perform_vdo_load(vdo);

	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		*reason = "Cannot load metadata from device";
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int suspend_vdo(struct vdo *vdo)
{
	int result = perform_vdo_suspend(vdo, !vdo->no_flush_suspend);
	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		log_error_strerror(result, "%s: Suspend device failed",
				   __func__);
		return result;
	}

	// Convert VDO_READ_ONLY to VDO_SUCCESS since a read-only suspension
	// still leaves the VDO suspended.
	return VDO_SUCCESS;
}

/**********************************************************************/
int resume_vdo(struct vdo *vdo)
{
	return perform_vdo_resume(vdo);
}

/**********************************************************************/
void finish_vdo(struct vdo *vdo)
{
	int i;

	for (i = 0; i < vdo->initialized_thread_count; i++) {
		finish_work_queue(vdo->threads[i].request_queue);
	}
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
struct sync_completion {
	struct vdo_completion vdo_completion;
	struct vdo *vdo;
	void *data;
	struct completion completion;
};

/**
 * Convert a vdo_completion to a sync completion.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a sync completion.
 **/
static inline struct sync_completion * __must_check
as_sync_completion(struct vdo_completion *completion)
{
	assert_vdo_completion_type(completion->type, SYNC_COMPLETION);
	return container_of(completion,
			    struct sync_completion,
			    vdo_completion);
}

/**
 * Initiate an arbitrary asynchronous base-code callback and wait for
 * it.
 *
 * An async queue operation is performed and we wait for completion.
 *
 * @param vdo        The vdo
 * @param action     The callback to launch
 * @param data       Unique data that can be used by the operation
 * @param thread_id  The thread on which to enqueue the operation
 **/
static void perform_vdo_operation(struct vdo *vdo,
				  vdo_action *action,
				  void *data,
				  thread_id_t thread_id)
{
	struct sync_completion sync;

	memset(&sync, 0, sizeof(sync));
	sync.vdo = vdo;
	initialize_vdo_completion(&sync.vdo_completion, vdo, SYNC_COMPLETION);
	init_completion(&sync.completion);

	sync.data = data;

	launch_vdo_completion_callback(&sync.vdo_completion, action, thread_id);
	wait_for_completion(&sync.completion);
}

/**********************************************************************/
struct vdo_compress_data {
	bool enable;
	bool was_enabled;
};

/**
 * Does the work of calling the base code to set compress state, then
 * tells the function waiting on completion to go ahead.
 *
 * @param completion  The completion
 **/
static void set_compressing_callback(struct vdo_completion *completion)
{
	struct sync_completion *sync = as_sync_completion(completion);
	struct vdo_compress_data *data =
		(struct vdo_compress_data *) sync->data;

	data->was_enabled = set_vdo_compressing(sync->vdo, data->enable);
	complete(&sync->completion);
}

/***********************************************************************/
bool set_kvdo_compressing(struct vdo *vdo, bool enable_compression)
{
	struct vdo_compress_data data;

	data.enable = enable_compression;
	perform_vdo_operation(vdo,
			      set_compressing_callback,
			      &data,
			      get_packer_zone_thread(get_thread_config(vdo)));
	return data.was_enabled;
}

/**********************************************************************/
struct vdo_read_only_data {
	int result;
};

/**********************************************************************/
static void enter_read_only_mode_callback(struct vdo_completion *completion)
{
	struct sync_completion *sync = as_sync_completion(completion);
	struct vdo_read_only_data *data = sync->data;

	vdo_enter_read_only_mode(sync->vdo->read_only_notifier, data->result);
	complete(&sync->completion);
}

/***********************************************************************/
void set_vdo_read_only(struct vdo *vdo, int result)
{
	struct vdo_read_only_data data;

	data.result = result;
	perform_vdo_operation(vdo,
			      enter_read_only_mode_callback,
			      &data,
			      get_admin_thread(get_thread_config(vdo)));
}

/**
 * Does the work of calling the vdo statistics gathering tool
 *
 * @param completion  The sync completion
 **/
static void get_vdo_statistics_callback(struct vdo_completion *completion)
{
	struct sync_completion *sync = as_sync_completion(completion);
	struct vdo_statistics *stats = (struct vdo_statistics *) sync->data;

	get_vdo_statistics(sync->vdo, stats);
	complete(&sync->completion);
}

/***********************************************************************/
void get_kvdo_statistics(struct vdo *vdo, struct vdo_statistics *stats)
{
	memset(stats, 0, sizeof(struct vdo_statistics));
	perform_vdo_operation(vdo,
			      get_vdo_statistics_callback,
			      stats,
			      get_admin_thread(get_thread_config(vdo)));
}

/**********************************************************************/
int vdo_resize_physical(struct vdo *vdo, block_count_t physical_count)
{
	int result = perform_grow_physical(vdo, physical_count);

	if (result != VDO_SUCCESS) {
		uds_log_error("resize operation failed, result = %d", result);
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int vdo_resize_logical(struct vdo *vdo, block_count_t logical_count)
{
	int result = perform_grow_logical(vdo, logical_count);

	if (result != VDO_SUCCESS) {
		uds_log_error("grow logical operation failed, result = %d",
			      result);
	}

	return result;
}

/**********************************************************************/
void enqueue_vdo_thread_work(struct vdo_thread *thread,
			     struct vdo_work_item *item)
{
	enqueue_work_queue(thread->request_queue, item);
}

/**********************************************************************/
void enqueue_vdo_work(struct vdo *vdo,
		      struct vdo_work_item *item,
		      thread_id_t thread_id)
{
	enqueue_vdo_thread_work(&vdo->threads[thread_id], item);
}

/**********************************************************************/
void enqueue_vio(struct vio *vio,
		 vdo_work_function work,
		 void *stats_function,
		 unsigned int action)
{
	thread_id_t thread_id = vio_as_completion(vio)->callback_thread_id;
	BUG_ON(thread_id >= vio->vdo->initialized_thread_count);
	launch_vio(vio,
		   work,
		   stats_function,
		   action,
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
			REQ_Q_ACTION_COMPLETION);
	enqueue_vdo_thread_work(&vdo->threads[thread_id],
				&completion->work_item);
}

/**********************************************************************/
thread_id_t get_callback_thread_id(void)
{
	struct vdo_thread *thread = get_work_queue_private_data();
	thread_id_t thread_id;

	if (thread == NULL) {
		return VDO_INVALID_THREAD_ID;
	}

	thread_id = thread->thread_id;

	if (PARANOID_THREAD_CONSISTENCY_CHECKS) {
		struct vdo *vdo = thread->vdo;
		struct kernel_layer *kernel_layer = vdo_as_kernel_layer(vdo);
		BUG_ON(&kernel_layer->vdo != vdo);
		BUG_ON(thread_id >= vdo->initialized_thread_count);
		BUG_ON(thread != &vdo->threads[thread_id]);
	}

	return thread_id;
}
