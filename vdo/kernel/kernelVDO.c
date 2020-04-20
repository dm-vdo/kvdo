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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.c#48 $
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
	struct kvdo_thread *thread = ptr;
	struct kvdo *kvdo = thread->kvdo;
	struct kernel_layer *layer = container_of(kvdo,
						  struct kernel_layer,
						  kvdo);
	registerAllocatingThread(&thread->allocating_thread,
				 &layer->allocations_allowed);
}

/**********************************************************************/
static void finish_kvdo_request_queue(void *ptr)
{
	unregisterAllocatingThread();
}

/**********************************************************************/
static const struct kvdo_work_queue_type request_queue_type = {
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
int initialize_kvdo(struct kvdo *kvdo,
		    const struct thread_config *thread_config,
		    char **reason)
{
	unsigned int base_threads = thread_config->base_thread_count;
	int result = ALLOCATE(base_threads,
			      struct kvdo_thread,
			      "request processing work queue",
			      &kvdo->threads);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocation thread structures";
		return result;
	}
	struct kernel_layer *layer = container_of(kvdo,
						  struct kernel_layer,
						  kvdo);
	for (kvdo->initialized_thread_count = 0;
	     kvdo->initialized_thread_count < base_threads;
	     kvdo->initialized_thread_count++) {
		struct kvdo_thread *thread =
			&kvdo->threads[kvdo->initialized_thread_count];

		thread->kvdo = kvdo;
		thread->thread_id = kvdo->initialized_thread_count;

		char queue_name[MAX_QUEUE_NAME_LEN];
		// Copy only LEN - 1 bytes and ensure NULL termination.
		get_vdo_thread_name(thread_config, kvdo->initialized_thread_count,
				 queue_name, sizeof(queue_name));
		int result = make_work_queue(layer->thread_name_prefix,
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
			while (kvdo->initialized_thread_count > 0) {
				unsigned int thread_to_destroy =
					kvdo->initialized_thread_count - 1;
				thread = &kvdo->threads[thread_to_destroy];
				finish_work_queue(thread->request_queue);
				free_work_queue(&thread->request_queue);
				kvdo->initialized_thread_count--;
			}
			FREE(kvdo->threads);
			return result;
		}
	}
	return VDO_SUCCESS;
}

/**********************************************************************/
int preload_kvdo(struct kvdo *kvdo,
		 PhysicalLayer *common,
		 const struct vdo_load_config *load_config,
		 bool vio_trace_recording,
		 char **reason)
{
	struct kernel_layer *layer = as_kernel_layer(common);

	init_completion(&layer->callbackSync);
	int result = prepare_to_load_vdo(kvdo->vdo, load_config);

	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		*reason = "Cannot load metadata from device";
		return result;
	}

	set_vdo_tracing_flags(kvdo->vdo, vio_trace_recording);
	return VDO_SUCCESS;
}

/**********************************************************************/
int start_kvdo(struct kvdo *kvdo, PhysicalLayer *common, char **reason)
{
	struct kernel_layer *layer = as_kernel_layer(common);

	init_completion(&layer->callbackSync);
	int result = perform_vdo_load(kvdo->vdo);

	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		*reason = "Cannot load metadata from device";
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int suspend_kvdo(struct kvdo *kvdo)
{
	if (kvdo->vdo == NULL) {
		return VDO_SUCCESS;
	}

	struct kernel_layer *layer = container_of(kvdo, struct kernel_layer,
						  kvdo);
	init_completion(&layer->callbackSync);
	int result = perform_vdo_suspend(kvdo->vdo, !layer->no_flush_suspend);

	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		char error_name[80] = "";
		char error_message[ERRBUF_SIZE] = "";

		logError("%s: Suspend device failed %d (%s: %s)",
			 __func__, result,
			 string_error_name(result, error_name,
					   sizeof(error_name)),
			 string_error(result, error_message,
				      sizeof(error_message)));
		return result;
	}

	// Convert VDO_READ_ONLY to VDO_SUCCESS since a read-only suspension
	// still leaves the VDO suspended.
	return VDO_SUCCESS;
}

/**********************************************************************/
int resume_kvdo(struct kvdo *kvdo)
{
	if (kvdo->vdo == NULL) {
		return VDO_SUCCESS;
	}

	struct kernel_layer *layer = container_of(kvdo, struct kernel_layer,
						  kvdo);
	init_completion(&layer->callbackSync);
	return perform_vdo_resume(kvdo->vdo);
}

/**********************************************************************/
void finish_kvdo(struct kvdo *kvdo)
{
	int i;

	for (i = 0; i < kvdo->initialized_thread_count; i++) {
		finish_work_queue(kvdo->threads[i].request_queue);
	}
}

/**********************************************************************/
void destroy_kvdo(struct kvdo *kvdo)
{
	destroy_vdo(kvdo->vdo);
	int i;

	for (i = 0; i < kvdo->initialized_thread_count; i++) {
		free_work_queue(&kvdo->threads[i].request_queue);
	}
	FREE(kvdo->threads);
	kvdo->threads = NULL;
}


/**********************************************************************/
void dump_kvdo_work_queue(struct kvdo *kvdo)
{
	int i;

	for (i = 0; i < kvdo->initialized_thread_count; i++) {
		dump_work_queue(kvdo->threads[i].request_queue);
	}
}

/**********************************************************************/
struct sync_queue_work {
	struct kvdo_work_item work_item;
	struct kvdo *kvdo;
	void *data;
	struct completion *completion;
};

/**
 * Initiate an arbitrary asynchronous base-code operation and wait for
 * it.
 *
 * An async queue operation is performed and we wait for completion.
 *
 * @param kvdo        The kvdo data handle
 * @param action      The operation to perform
 * @param data        Unique data that can be used by the operation
 * @param thread_id   The thread on which to perform the operation
 * @param completion  The completion to wait on
 *
 * @return VDO_SUCCESS of an error code
 **/
static void performKVDOOperation(struct kvdo *kvdo,
				 KvdoWorkFunction action,
				 void *data,
				 ThreadID thread_id,
				 struct completion *completion)
{
	struct sync_queue_work sync;

	memset(&sync, 0, sizeof(sync));
	setup_work_item(&sync.work_item, action, NULL, REQ_Q_ACTION_SYNC);
	sync.kvdo = kvdo;
	sync.data = data;
	sync.completion = completion;

	init_completion(completion);
	enqueue_kvdo_work(kvdo, &sync.work_item, thread_id);
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
static void set_compressing_work(struct kvdo_work_item *item)
{
	struct sync_queue_work *work =
		container_of(item, struct sync_queue_work, work_item);
	struct vdo_compress_data *data = (struct vdo_compress_data *) work->data;

	data->was_enabled =
		set_vdo_compressing(get_vdo(work->kvdo), data->enable);
	complete(work->completion);
}

/***********************************************************************/
bool set_kvdo_compressing(struct kvdo *kvdo, bool enable_compression)
{
	struct completion compress_wait;
	struct vdo_compress_data data;

	data.enable = enable_compression;
	performKVDOOperation(kvdo,
			     set_compressing_work,
			     &data,
			     get_packer_zone_thread(get_thread_config(kvdo->vdo)),
			     &compress_wait);
	return data.was_enabled;
}

/**********************************************************************/
struct vdo_read_only_data {
	int result;
};

/**********************************************************************/
static void enter_read_only_mode_work(struct kvdo_work_item *item)
{
	struct sync_queue_work *work =
		container_of(item, struct sync_queue_work, work_item);
	struct vdo_read_only_data *data = work->data;

	make_vdo_read_only(get_vdo(work->kvdo), data->result);
	complete(work->completion);
}

/***********************************************************************/
void set_kvdo_read_only(struct kvdo *kvdo, int result)
{
	struct completion read_only_wait;
	struct vdo_read_only_data data;

	data.result = result;
	performKVDOOperation(kvdo, enter_read_only_mode_work, &data,
			     get_admin_thread(get_thread_config(kvdo->vdo)),
			     &read_only_wait);
}

/**
 * Does the work of calling the vdo statistics gathering tool
 *
 * @param item   The work item
 **/
static void get_vdo_statistics_work(struct kvdo_work_item *item)
{
	struct sync_queue_work *work =
		container_of(item, struct sync_queue_work, work_item);
	struct vdo_statistics *stats = (struct vdo_statistics *) work->data;

	get_vdo_statistics(get_vdo(work->kvdo), stats);
	complete(work->completion);
}

/***********************************************************************/
void get_kvdo_statistics(struct kvdo *kvdo, struct vdo_statistics *stats)
{
	struct completion stats_wait;

	memset(stats, 0, sizeof(struct vdo_statistics));
	performKVDOOperation(kvdo,
			     get_vdo_statistics_work,
			     stats,
			     get_admin_thread(get_thread_config(kvdo->vdo)),
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
static void perform_vdo_action_work(struct kvdo_work_item *item)
{
	struct sync_queue_work *work =
		container_of(item, struct sync_queue_work, work_item);
	struct vdo_action_data *data = work->data;
	ThreadID id = getCallbackThreadID();

	set_callback_with_parent(data->vdo_completion,
			        finish_vdo_action,
			        id,
			        work);
	data->action(data->vdo_completion);
}

/**********************************************************************/
int perform_kvdo_extended_command(struct kvdo *kvdo, int argc, char **argv)
{
	struct vdo_action_data data;
	struct vdo_command_completion cmd;

	int result =
		initialize_vdo_command_completion(&cmd, get_vdo(kvdo),
						  argc, argv);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initialize_vdo_action_data(&data,
				   execute_vdo_extended_command,
				   &cmd.completion);
	performKVDOOperation(kvdo,
			     perform_vdo_action_work,
			     &data,
			     get_admin_thread(get_thread_config(kvdo->vdo)),
			     &data.waiter);

	return destroy_vdo_command_completion(&cmd);
}

/**********************************************************************/
void dump_kvdo_status(struct kvdo *kvdo)
{
	dump_vdo_status(kvdo->vdo);
}

/**********************************************************************/
bool get_kvdo_compressing(struct kvdo *kvdo)
{
	return get_vdo_compressing(kvdo->vdo);
}

/**********************************************************************/
int kvdo_prepare_to_grow_physical(struct kvdo *kvdo,
				  block_count_t physical_count)
{
	struct vdo *vdo = kvdo->vdo;

	return prepare_to_grow_physical(vdo, physical_count);
}

/**********************************************************************/
int kvdo_resize_physical(struct kvdo *kvdo, block_count_t physical_count)
{
	struct kernel_layer *layer = container_of(kvdo,
						  struct kernel_layer,
						  kvdo);
	init_completion(&layer->callbackSync);
	int result = perform_grow_physical(kvdo->vdo, physical_count);

	if (result != VDO_SUCCESS) {
		logError("resize operation failed, result = %d", result);
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int kvdo_prepare_to_grow_logical(struct kvdo *kvdo, block_count_t logical_count)
{
	struct vdo *vdo = kvdo->vdo;

	return prepare_to_grow_logical(vdo, logical_count);
}

/**********************************************************************/
int kvdo_resize_logical(struct kvdo *kvdo, block_count_t logical_count)
{
	struct kernel_layer *layer = container_of(kvdo,
						  struct kernel_layer,
						  kvdo);
	init_completion(&layer->callbackSync);
	int result = perform_grow_logical(kvdo->vdo, logical_count);

	if (result != VDO_SUCCESS) {
		logError("grow logical operation failed, result = %d", result);
	}

	return result;
}

/**********************************************************************/
write_policy get_kvdo_write_policy(struct kvdo *kvdo)
{
	return get_write_policy(kvdo->vdo);
}

/**********************************************************************/
void enqueue_kvdo_thread_work(struct kvdo_thread *thread,
			      struct kvdo_work_item *item)
{
	enqueue_work_queue(thread->request_queue, item);
}

/**********************************************************************/
void enqueue_kvdo_work(struct kvdo *kvdo, struct kvdo_work_item *item,
		       ThreadID thread_id)
{
	enqueue_kvdo_thread_work(&kvdo->threads[thread_id], item);
}

/**********************************************************************/
void enqueue_kvio(struct kvio *kvio, KvdoWorkFunction work,
		  void *stats_function, unsigned int action)
{
	ThreadID thread_id = vio_as_completion(kvio->vio)->callbackThreadID;

	BUG_ON(thread_id >= kvio->layer->kvdo.initialized_thread_count);
	launch_kvio(kvio,
		    work,
		    stats_function,
		    action,
		    kvio->layer->kvdo.threads[thread_id].request_queue);
}

/**********************************************************************/
static void kvdo_enqueue_work(struct kvdo_work_item *work_item)
{
	run_callback(container_of(work_item, struct vdo_completion,
				  work_item));
}

/**********************************************************************/
void kvdo_enqueue(struct vdo_completion *completion)
{
	struct kernel_layer *layer = as_kernel_layer(completion->layer);
	ThreadID thread_id = completion->callbackThreadID;

	if (ASSERT(thread_id < layer->kvdo.initialized_thread_count,
		   "thread_id %u (completion type %d) is less than thread count %u",
		   thread_id,
		   completion->type,
		   layer->kvdo.initialized_thread_count) != UDS_SUCCESS) {
		BUG();
	}

	if (completion->type == VIO_COMPLETION) {
		vio_add_trace_record(as_vio(completion),
				     THIS_LOCATION("$F($cb)"));
	}
	setup_work_item(&completion->work_item, kvdo_enqueue_work,
			(KvdoWorkFunction) completion->callback,
			REQ_Q_ACTION_COMPLETION);
	enqueue_kvdo_thread_work(&layer->kvdo.threads[thread_id],
				 &completion->work_item);
}

/**********************************************************************/
ThreadID getCallbackThreadID(void)
{
	struct kvdo_thread *thread = get_work_queue_private_data();

	if (thread == NULL) {
		return INVALID_THREAD_ID;
	}

	ThreadID thread_id = thread->thread_id;

	if (PARANOID_THREAD_CONSISTENCY_CHECKS) {
		struct kvdo *kvdo = thread->kvdo;
		struct kernel_layer *kernel_layer =
			container_of(kvdo, struct kernel_layer, kvdo);
		BUG_ON(&kernel_layer->kvdo != kvdo);
		BUG_ON(thread_id >= kvdo->initialized_thread_count);
		BUG_ON(thread != &kvdo->threads[thread_id]);
	}
	return thread_id;
}
