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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelLayer.c#232 $
 */

#include "kernelLayer.h"

#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/lz4.h>
#include <linux/ratelimit.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"
#include "permassert.h"

#include "adminCompletion.h"
#include "adminState.h"
#include "flush.h"
#include "releaseVersions.h"
#include "statistics.h"
#include "vdo.h"
#include "vdoLoad.h"
#include "vdoResize.h"
#include "vdoResizeLogical.h"
#include "vdoSuspend.h"
#include "volumeGeometry.h"

#include "bio.h"
#include "dataKVIO.h"
#include "dedupeIndex.h"
#include "deviceConfig.h"
#include "deviceRegistry.h"
#include "instanceNumber.h"
#include "ioSubmitter.h"
#include "kvio.h"
#include "poolSysfs.h"
#include "stringUtils.h"
#include "vdoInit.h"

static const struct vdo_work_queue_type bio_ack_q_type = {
	.action_table = {
		{
			.name = "bio_ack",
			.code = BIO_ACK_Q_ACTION_ACK,
			.priority = 0
		},
	},
};

static const struct vdo_work_queue_type cpu_q_type = {
	.action_table = {
		{
			.name = "cpu_complete_vio",
			.code = CPU_Q_ACTION_COMPLETE_VIO,
			.priority = 0
		},
		{
			.name = "cpu_compress_block",
			.code = CPU_Q_ACTION_COMPRESS_BLOCK,
			.priority = 0
		},
		{
			.name = "cpu_hash_block",
			.code = CPU_Q_ACTION_HASH_BLOCK,
			.priority = 0
		},
		{
			.name = "cpu_event_reporter",
			.code = CPU_Q_ACTION_EVENT_REPORTER,
			.priority = 0
		},
	},
};

/**
 * Start processing a new data vio based on the supplied bio, but from within
 * a VDO thread context, when we're not allowed to block. Using this path at
 * all suggests a bug or erroneous usage, but we special-case it to avoid a
 * deadlock that can apparently result. Message will be logged to alert the
 * administrator that something has gone wrong, while we attempt to continue
 * processing other requests.
 *
 * If a request permit can be acquired immediately,
 * vdo_launch_data_vio_from_bio will be called. (If the bio is a discard
 * operation, a permit from the discard limiter will be requested but the call
 * will be made with or without it.) If the request permit is not available,
 * the bio will be saved on a list to be launched later. Either way, this
 * function will not block, and will take responsibility for processing the
 * bio.
 *
 * @param vdo              The vdo
 * @param bio              The bio to launch
 * @param arrival_jiffies  The arrival time of the bio
 *
 * @return DM_MAPIO_SUBMITTED or a system error code
 **/
static int launch_data_vio_from_vdo_thread(struct vdo *vdo,
					   struct bio *bio,
					   uint64_t arrival_jiffies)
{
	bool has_discard_permit;
	int result;

	uds_log_warning("vdo_launch_bio called from within a VDO thread!");
	/*
	 * We're not yet entirely sure what circumstances are causing this
	 * situation in [ESC-638], but it does appear to be happening and
	 * causing VDO to deadlock.
	 *
	 * Somehow vdo_launch_bio is being called from generic_make_request
	 * which is being called from the VDO code to pass a flush on down to
	 * the underlying storage system; we've got 2000 requests in progress,
	 * so we have to wait for one to complete, but none can complete while
	 * the bio thread is blocked from passing more I/O requests down. Near
	 * as we can tell, the flush bio should always have gotten updated to
	 * point to the storage system, so we shouldn't be calling back into
	 * VDO unless something's gotten messed up somewhere.
	 *
	 * To side-step this case, if the limiter says we're busy *and* we're
	 * running on one of VDO's own threads, we'll drop the I/O request in a
	 * special queue for processing as soon as vios become free.
	 *
	 * We don't want to do this in general because it leads to unbounded
	 * buffering, arbitrarily high latencies, inability to push back in a
	 * way the caller can take advantage of, etc. If someone wants huge
	 * amounts of buffering on top of VDO, they're welcome to access it
	 * through the kernel page cache or roll their own.
	 */
	if (!limiter_poll(&vdo->request_limiter)) {
		add_to_vdo_deadlock_queue(&vdo->deadlock_queue,
					  bio,
					  arrival_jiffies);
		uds_log_warning("queued an I/O request to avoid deadlock!");

		return DM_MAPIO_SUBMITTED;
	}

	has_discard_permit =
		((bio_op(bio) == REQ_OP_DISCARD) &&
		 limiter_poll(&vdo->discard_limiter));
	result = vdo_launch_data_vio_from_bio(vdo,
					      bio,
					      arrival_jiffies,
					      has_discard_permit);
	// Succeed or fail, vdo_launch_data_vio_from_bio owns the permit(s)
	// now.
	if (result != VDO_SUCCESS) {
		return result;
	}

	return DM_MAPIO_SUBMITTED;
}

/**********************************************************************/
int vdo_launch_bio(struct vdo *vdo, struct bio *bio)
{
	int result;
	uint64_t arrival_jiffies = jiffies;
	struct vdo_work_queue *current_work_queue;
	bool has_discard_permit = false;
	const struct admin_state_code *code
		= get_vdo_admin_state_code(&vdo->admin_state);

	ASSERT_LOG_ONLY(code->normal,
			"vdo_launch_bio should not be called while in state %s",
			code->name);

	// Count all incoming bios.
	vdo_count_bios(&vdo->stats.bios_in, bio);


	// Handle empty bios.  Empty flush bios are not associated with a vio.
	if ((bio_op(bio) == REQ_OP_FLUSH) ||
	    ((bio->bi_opf & REQ_PREFLUSH) != 0)) {
		launch_vdo_flush(vdo, bio);
		return DM_MAPIO_SUBMITTED;
	}

	current_work_queue = get_current_work_queue();

	if ((current_work_queue != NULL) &&
	    (vdo == get_work_queue_owner(current_work_queue))) {
		/*
		 * This prohibits sleeping during I/O submission to VDO from
		 * its own thread.
		 */
		return launch_data_vio_from_vdo_thread(vdo,
						       bio,
						       arrival_jiffies);
	}

	if (bio_op(bio) == REQ_OP_DISCARD) {
		limiter_wait_for_one_free(&vdo->discard_limiter);
		has_discard_permit = true;
	}
	limiter_wait_for_one_free(&vdo->request_limiter);

	result = vdo_launch_data_vio_from_bio(vdo,
					      bio,
					      arrival_jiffies,
					      has_discard_permit);
	// Succeed or fail, vdo_launch_data_vio_from_bio owns the permit(s)
	// now.
	if (result != VDO_SUCCESS) {
		return result;
	}

	return DM_MAPIO_SUBMITTED;
}

/**********************************************************************/
void complete_many_requests(struct vdo *vdo, uint32_t count)
{
	// If we had to buffer some requests to avoid deadlock, release them
	// now.
	while (count > 0) {
		bool has_discard_permit;
		int result;
		uint64_t arrival_jiffies = 0;
		struct bio *bio = poll_vdo_deadlock_queue(&vdo->deadlock_queue,
							  &arrival_jiffies);
		if (likely(bio == NULL)) {
			break;
		}

		has_discard_permit =
			((bio_op(bio) == REQ_OP_DISCARD) &&
			 limiter_poll(&vdo->discard_limiter));
		result = vdo_launch_data_vio_from_bio(vdo,
						      bio,
						      arrival_jiffies,
						      has_discard_permit);
		if (result != VDO_SUCCESS) {
			vdo_complete_bio(bio, result);
		}
		// Succeed or fail, vdo_launch_data_vio_from_bio owns the
		// permit(s) now.
		count--;
	}
	// Notify the limiter, so it can wake any blocked processes.
	if (count > 0) {
		limiter_release_many(&vdo->request_limiter, count);
	}
}

/**********************************************************************/
int make_kernel_layer(unsigned int instance,
		      struct device_config *config,
		      char **reason,
		      struct vdo **vdo_ptr)
{
	int result;
	struct vdo *vdo;
	char thread_name_prefix[MAX_VDO_WORK_QUEUE_NAME_LEN];

	// VDO-3769 - Set a generic reason so we don't ever return garbage.
	*reason = "Unspecified error";

	result = UDS_ALLOCATE(1, struct vdo, __func__, &vdo);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate VDO";
		release_vdo_instance(instance);
		return result;
	}

	result = initialize_vdo(vdo, config, instance, reason);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// From here on, the caller will clean up if there is an error.
	*vdo_ptr = vdo;

	snprintf(thread_name_prefix,
		 sizeof(thread_name_prefix),
		 "%s%u",
		 THIS_MODULE->name,
		 instance);

	result = make_batch_processor(vdo,
				      return_data_vio_batch_to_pool,
				      vdo,
				      &vdo->data_vio_releaser);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate vio-freeing batch processor";
		return result;
	}

	// Dedupe Index
	BUG_ON(thread_name_prefix[0] == '\0');
	result = make_vdo_dedupe_index(&vdo->dedupe_index,
				       vdo,
				       thread_name_prefix);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot initialize dedupe index";
		return result;
	}

	/*
	 * Part 3 - Do initializations that depend upon other previous
	 * initializations, but have no order dependencies at freeing time.
	 * Order dependencies for initialization are identified using BUG_ON.
	 */


	// Data vio pool
	BUG_ON(vdo->device_config->logical_block_size <= 0);
	BUG_ON(vdo->request_limiter.limit <= 0);
	BUG_ON(vdo->device_config->owned_device == NULL);
	result = make_data_vio_buffer_pool(vdo->request_limiter.limit,
					   &vdo->data_vio_pool);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocate vio data";
		return result;
	}

	/*
	 * Part 4 - Do initializations that depend upon other previous
	 * initialization, that may have order dependencies at freeing time.
	 * These are mostly starting up the workqueue threads.
	 */

	// Base-code thread, etc
	result = make_vdo_threads(vdo, thread_name_prefix, reason);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Bio queue
	result = make_vdo_io_submitter(thread_name_prefix,
				       config->thread_counts.bio_threads,
				       config->thread_counts.bio_rotation_interval,
				       vdo->request_limiter.limit,
				       vdo,
				       &vdo->io_submitter);
	if (result != VDO_SUCCESS) {
		*reason = "bio submission initialization failed";
		return result;
	}

	// Bio ack queue
	if (use_bio_ack_queue(vdo)) {
		result = make_work_queue(thread_name_prefix,
					 "ackQ",
					 &vdo->work_queue_directory,
					 vdo,
					 vdo,
					 &bio_ack_q_type,
					 config->thread_counts.bio_ack_threads,
					 NULL,
					 &vdo->bio_ack_queue);
		if (result != VDO_SUCCESS) {
			*reason = "bio ack queue initialization failed";
			return result;
		}
	}

	// CPU Queues
	result = make_work_queue(thread_name_prefix,
				 "cpuQ",
				 &vdo->work_queue_directory,
				 vdo,
				 vdo,
				 &cpu_q_type,
				 config->thread_counts.cpu_threads,
				 (void **) vdo->compression_context,
				 &vdo->cpu_queue);
	if (result != VDO_SUCCESS) {
		*reason = "CPU queue initialization failed";
		return result;
	}

	return VDO_SUCCESS;
}

