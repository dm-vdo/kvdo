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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.c#71 $
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

#include "physicalLayer.h"
#include "statistics.h"
#include "threadConfig.h"
#include "vdo.h"
#include "vdoDebug.h"
//#include "vdoInternal.h"
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
static void start_kvdo_request_queue(void *ptr)
{
	struct vdo_thread *thread = ptr;
	struct vdo *vdo = thread->vdo;
	struct kernel_layer *layer = as_kernel_layer(vdo->layer);
	register_allocating_thread(&thread->allocating_thread,
				   &layer->allocations_allowed);
}

/**********************************************************************/
static void finish_kvdo_request_queue(void *ptr)
{
	unregister_allocating_thread();
}

/**********************************************************************/
static const struct vdo_work_queue_type request_queue_type = {
	.start = start_kvdo_request_queue,
	.finish = finish_kvdo_request_queue,
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
int make_vdo_threads(struct vdo *vdo,
		     const struct thread_config *thread_config,
		     char **reason)
{
	struct kernel_layer *layer = as_kernel_layer(vdo->layer);
	unsigned int base_threads = thread_config->base_thread_count;
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
		get_vdo_thread_name(thread_config,
				    vdo->initialized_thread_count,
				    queue_name,
				    sizeof(queue_name));
		result = make_work_queue(layer->thread_name_prefix,
					 queue_name,
					 &layer->wq_directory,
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
			return result;
		}
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int preload_kvdo(struct vdo *vdo,
		 PhysicalLayer *common,
		 const struct vdo_load_config *load_config,
		 bool vio_trace_recording,
		 char **reason)
{
	int result;
	struct kernel_layer *layer = as_kernel_layer(common);

	init_completion(&layer->callback_sync);
	result = prepare_to_load_vdo(vdo, load_config);

	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		*reason = "Cannot load metadata from device";
		return result;
	}

	set_vdo_tracing_flags(vdo, vio_trace_recording);
	return VDO_SUCCESS;
}

/**********************************************************************/
int start_kvdo(struct vdo *vdo, PhysicalLayer *common, char **reason)
{
	int result;
	struct kernel_layer *layer = as_kernel_layer(common);

	init_completion(&layer->callback_sync);
	result = perform_vdo_load(vdo);

	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		*reason = "Cannot load metadata from device";
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int suspend_kvdo(struct vdo *vdo)
{
	struct kernel_layer *layer = as_kernel_layer(vdo->layer);
	int result;

	init_completion(&layer->callback_sync);
	result = perform_vdo_suspend(vdo, !layer->no_flush_suspend);
	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		char error_name[80] = "";
		char error_message[ERRBUF_SIZE] = "";

		uds_log_error("%s: Suspend device failed %d (%s: %s)",
			      __func__, result,
			      string_error_name(result, error_name,
						sizeof(error_name)),
			      uds_string_error(result, error_message,
					       sizeof(error_message)));
		return result;
	}

	// Convert VDO_READ_ONLY to VDO_SUCCESS since a read-only suspension
	// still leaves the VDO suspended.
	return VDO_SUCCESS;
}

/**********************************************************************/
int resume_kvdo(struct vdo *vdo)
{
	struct kernel_layer *layer = as_kernel_layer(vdo->layer);

	init_completion(&layer->callback_sync);
	return perform_vdo_resume(vdo);
}

/**********************************************************************/
void finish_kvdo(struct vdo *vdo)
{
	int i;

	for (i = 0; i < vdo->initialized_thread_count; i++) {
		finish_work_queue(vdo->threads[i].request_queue);
	}
}

/**********************************************************************/
void destroy_kvdo(struct vdo *vdo)
{
	int i;
	destroy_vdo(vdo);

	for (i = 0; i < vdo->initialized_thread_count; i++) {
		free_work_queue(&vdo->threads[i].request_queue);
	}
	FREE(vdo->threads);
	vdo->threads = NULL;
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
struct sync_queue_work {
	struct vdo_work_item work_item;
	struct vdo *vdo;
	void *data;
	struct completion *completion;
};

/**
 * Initiate an arbitrary asynchronous base-code operation and wait for
 * it.
 *
 * An async queue operation is performed and we wait for completion.
 *
 * @param vdo         The vdo data handle
 * @param action      The operation to perform
 * @param data        Unique data that can be used by the operation
 * @param thread_id   The thread on which to perform the operation
 * @param completion  The completion to wait on
 **/
static void perform_kvdo_operation(struct vdo *vdo,
				   vdo_work_function action,
				   void *data,
				   thread_id_t thread_id,
				   struct completion *completion)
{
	struct sync_queue_work sync;

	memset(&sync, 0, sizeof(sync));
	setup_work_item(&sync.work_item, action, NULL, REQ_Q_ACTION_SYNC);
	sync.vdo = vdo;
	sync.data = data;
	sync.completion = completion;

	init_completion(completion);
	enqueue_vdo_work(vdo, &sync.work_item, thread_id);
	wait_for_completion(completion);
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
 * @param item  The work item
 **/
static void set_compressing_work(struct vdo_work_item *item)
{
	struct sync_queue_work *work =
		container_of(item, struct sync_queue_work, work_item);
	struct vdo_compress_data *data = (struct vdo_compress_data *) work->data;

	data->was_enabled = set_vdo_compressing(work->vdo, data->enable);
	complete(work->completion);
}

/***********************************************************************/
bool set_kvdo_compressing(struct vdo *vdo, bool enable_compression)
{
	struct completion compress_wait;
	struct vdo_compress_data data;

	data.enable = enable_compression;
	perform_kvdo_operation(vdo,
			       set_compressing_work,
			       &data,
			       get_packer_zone_thread(get_thread_config(vdo)),
			       &compress_wait);
	return data.was_enabled;
}

/**********************************************************************/
struct vdo_read_only_data {
	int result;
};

/**********************************************************************/
static void enter_read_only_mode_work(struct vdo_work_item *item)
{
	struct sync_queue_work *work =
		container_of(item, struct sync_queue_work, work_item);
	struct vdo_read_only_data *data = work->data;

	make_vdo_read_only(work->vdo, data->result);
	complete(work->completion);
}

/***********************************************************************/
void set_kvdo_read_only(struct vdo *vdo, int result)
{
	struct completion read_only_wait;
	struct vdo_read_only_data data;

	data.result = result;
	perform_kvdo_operation(vdo, enter_read_only_mode_work, &data,
			       get_admin_thread(get_thread_config(vdo)),
			       &read_only_wait);
}

/**
 * Does the work of calling the vdo statistics gathering tool
 *
 * @param item   The work item
 **/
static void get_vdo_statistics_work(struct vdo_work_item *item)
{
	struct sync_queue_work *work =
		container_of(item, struct sync_queue_work, work_item);
	struct vdo_statistics *stats = (struct vdo_statistics *) work->data;

	get_vdo_statistics(work->vdo, stats);
	complete(work->completion);
}

/***********************************************************************/
void get_kvdo_statistics(struct vdo *vdo, struct vdo_statistics *stats)
{
	struct completion stats_wait;

	memset(stats, 0, sizeof(struct vdo_statistics));
	perform_kvdo_operation(vdo,
			       get_vdo_statistics_work,
			       stats,
			       get_admin_thread(get_thread_config(vdo)),
			       &stats_wait);
}

/**
 * A structure to invoke an arbitrary VDO action.
 **/
struct vdo_action_data {
	vdo_action *action;
	struct vdo_completion *vdo_completion;
	struct completion waiter;
};

/**
 * Initialize a struct vdo_action_data structure so that the specified
 * action can be invoked on the specified completion.
 *
 * @param data               A vdo_action_data structure.
 * @param action             The vdo_action to execute.
 * @param vdo_completion     The VDO completion upon which the action acts.
 **/
static void initialize_vdo_action_data(struct vdo_action_data *data,
				       vdo_action *action,
				       struct vdo_completion *vdo_completion)
{
	*data = (struct vdo_action_data) {
		.action = action,
		.vdo_completion = vdo_completion,
	};
}

/**
 * The VDO callback that completes the kvdo completion.
 *
 * @param vdo_completion     The VDO completion which was acted upon.
 **/
static void finish_vdo_action(struct vdo_completion *vdo_completion)
{
	struct sync_queue_work *work = vdo_completion->parent;

	complete(work->completion);
}

/**
 * Perform a VDO base code action as specified by a vdo_action_data
 * structure.
 *
 * Sets the completion callback and parent inside the vdo_action_data
 * structure so that the corresponding kernel completion is completed
 * when the VDO completion is.
 *
 * @param item          A kvdo work queue item.
 **/
static void perform_vdo_action_work(struct vdo_work_item *item)
{
	struct sync_queue_work *work =
		container_of(item, struct sync_queue_work, work_item);
	struct vdo_action_data *data = work->data;
	thread_id_t id = get_callback_thread_id();

	set_callback_with_parent(data->vdo_completion,
			        finish_vdo_action,
			        id,
			        work);
	data->action(data->vdo_completion);
}

/**********************************************************************/
int perform_kvdo_extended_command(struct vdo *vdo, int argc, char **argv)
{
	struct vdo_action_data data;
	struct vdo_command_completion cmd;
	initialize_vdo_command_completion(&cmd, vdo, argc, argv);
	initialize_vdo_action_data(&data,
				   execute_vdo_extended_command,
				   &cmd.completion);
	perform_kvdo_operation(vdo,
			       perform_vdo_action_work,
			       &data,
			       get_admin_thread(get_thread_config(vdo)),
			       &data.waiter);
	return cmd.completion.result;
}

/**********************************************************************/
void dump_kvdo_status(struct vdo *vdo)
{
	dump_vdo_status(vdo);
}

/**********************************************************************/
int kvdo_resize_physical(struct vdo *vdo, block_count_t physical_count)
{
	int result;
	struct kernel_layer *layer = as_kernel_layer(vdo->layer);

	init_completion(&layer->callback_sync);
	result = perform_grow_physical(vdo, physical_count);

	if (result != VDO_SUCCESS) {
		uds_log_error("resize operation failed, result = %d", result);
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int kvdo_resize_logical(struct vdo *vdo, block_count_t logical_count)
{
	int result;
	struct kernel_layer *layer = as_kernel_layer(vdo->layer);

	init_completion(&layer->callback_sync);
	result = perform_grow_logical(vdo, logical_count);

	if (result != VDO_SUCCESS) {
		uds_log_error("grow logical operation failed, result = %d",
			      result);
	}

	return result;
}

/**********************************************************************/
void enqueue_kvdo_thread_work(struct vdo_thread *thread,
			      struct vdo_work_item *item)
{
	enqueue_work_queue(thread->request_queue, item);
}

/**********************************************************************/
void enqueue_vdo_work(struct vdo *vdo,
		      struct vdo_work_item *item,
		      thread_id_t thread_id)
{
	enqueue_kvdo_thread_work(&vdo->threads[thread_id], item);
}

/**********************************************************************/
void enqueue_vio(struct vio *vio,
		 vdo_work_function work,
		 void *stats_function,
		 unsigned int action)
{
	struct vdo_completion *completion = vio_as_completion(vio);
	thread_id_t thread_id = completion->callback_thread_id;
	struct kernel_layer *layer = as_kernel_layer(completion->layer);
	BUG_ON(thread_id >= layer->vdo.initialized_thread_count);
	launch_vio(vio,
		   work,
		   stats_function,
		   action,
		   layer->vdo.threads[thread_id].request_queue);
}

/**********************************************************************/
static void kvdo_enqueue_work(struct vdo_work_item *work_item)
{
	run_callback(container_of(work_item, struct vdo_completion,
				  work_item));
}

/**********************************************************************/
void kvdo_enqueue(struct vdo_completion *completion)
{
	struct kernel_layer *layer = as_kernel_layer(completion->layer);
	thread_id_t thread_id = completion->callback_thread_id;

	if (ASSERT(thread_id < layer->vdo.initialized_thread_count,
		   "thread_id %u (completion type %d) is less than thread count %u",
		   thread_id,
		   completion->type,
		   layer->vdo.initialized_thread_count) != UDS_SUCCESS) {
		BUG();
	}

	setup_work_item(&completion->work_item, kvdo_enqueue_work,
			completion->callback,
			REQ_Q_ACTION_COMPLETION);
	enqueue_kvdo_thread_work(&layer->vdo.threads[thread_id],
				 &completion->work_item);
}

/**********************************************************************/
thread_id_t get_callback_thread_id(void)
{
	struct vdo_thread *thread = get_work_queue_private_data();
	thread_id_t thread_id;

	if (thread == NULL) {
		return INVALID_THREAD_ID;
	}

	thread_id = thread->thread_id;

	if (PARANOID_THREAD_CONSISTENCY_CHECKS) {
		struct vdo *vdo = thread->vdo;
		struct kernel_layer *kernel_layer =
			as_kernel_layer(vdo->layer);
		BUG_ON(&kernel_layer->vdo != vdo);
		BUG_ON(thread_id >= vdo->initialized_thread_count);
		BUG_ON(thread != &vdo->threads[thread_id]);
	}

	return thread_id;
}
