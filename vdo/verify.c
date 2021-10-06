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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/verify.c#36 $
 */

#include "logger.h"
#include "permassert.h"

#include "comparisons.h"
#include "dataKVIO.h"
#include <asm/unaligned.h>

/**
 * Verify the deduplication advice from the UDS index, and invoke a
 * callback once the answer is available.
 *
 * After we've compared the stored data with the data to be written,
 * or after we've failed to be able to do so, the stored VIO callback
 * is queued to be run in the main (kvdoReqQ) thread.
 *
 * If the advice turns out to be stale and the deduplication session
 * is still active, submit a correction.  (Currently the correction
 * must be sent before the callback can be invoked, if the dedupe
 * session is still live.)
 *
 * @param item  The workitem from the queue
 **/
static void verify_duplication_work(struct vdo_work_item *item)
{
	struct data_vio *data_vio = work_item_as_data_vio(item);

	if (likely(memory_equal(data_vio->data_block,
				data_vio->read_block.data,
				VDO_BLOCK_SIZE))) {
		// Leave data_vio->is_duplicate set to true.
	} else {
		data_vio->is_duplicate = false;
	}

	enqueue_data_vio_callback(data_vio);
}

/**
 * Verify the deduplication advice from the UDS index, and invoke a
 * callback once the answer is available.
 *
 * @param completion  The data_vio that we are looking to dedupe.
 **/
static void verify_read_block_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	int err = data_vio->read_block.status;

	if (unlikely(err != 0)) {
		uds_log_debug("%s: err %d", __func__, err);
		data_vio->is_duplicate = false;
		enqueue_data_vio_callback(data_vio);
		return;
	}

	launch_data_vio_on_cpu_queue(data_vio,
				     verify_duplication_work,
				     CPU_Q_COMPRESS_BLOCK_PRIORITY);
}

/**********************************************************************/
void verify_data_vio_duplication(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(data_vio->is_duplicate,
			"advice to verify must be valid");
	ASSERT_LOG_ONLY(data_vio->duplicate.state != VDO_MAPPING_STATE_UNMAPPED,
			"advice to verify must not be a discard");
	ASSERT_LOG_ONLY(data_vio->duplicate.pbn != VDO_ZERO_BLOCK,
			"advice to verify must not point to the zero block");
	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"zeroed block should not have advice to verify");

	vdo_read_block(data_vio,
		       data_vio->duplicate.pbn,
		       data_vio->duplicate.state,
		       BIO_Q_VERIFY_PRIORITY,
		       verify_read_block_callback);
}

/**********************************************************************/
bool compare_data_vios(struct data_vio *first, struct data_vio *second)
{
	return memory_equal(first->data_block, second->data_block,
			    VDO_BLOCK_SIZE);
}
