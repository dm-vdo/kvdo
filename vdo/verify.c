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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/verify.c#22 $
 */

#include "physicalLayer.h"

#include "logger.h"

#include "dataKVIO.h"
#include <asm/unaligned.h>

/**
 * Compare blocks of memory for equality.
 *
 * This assumes the blocks are likely to be large; it's not well
 * optimized for comparing just a few bytes.  This is desirable
 * because the Linux kernel memcmp() routine on x86 is not well
 * optimized for large blocks, and the performance penalty turns out
 * to be significant if you're doing lots of 4KB comparisons.
 *
 * @param pointer_argument1  first data block
 * @param pointer_argument2  second data block
 * @param length             length of the data block
 *
 * @return   true iff the two blocks are equal
 **/
static bool __must_check
memory_equal(void *pointer_argument1, void *pointer_argument2, size_t length)
{
	byte *pointer1 = pointer_argument1;
	byte *pointer2 = pointer_argument2;

	while (length >= sizeof(uint64_t)) {
		/*
		 * get_unaligned is just for paranoia.  (1) On x86_64 it is
		 * treated the same as an aligned access.  (2) In this use case,
		 * one or both of the inputs will almost(?) always be aligned.
		 */
		if (get_unaligned((u64 *) pointer1) !=
		    get_unaligned((u64 *) pointer2)) {
			return false;
		}
		pointer1 += sizeof(uint64_t);
		pointer2 += sizeof(uint64_t);
		length -= sizeof(uint64_t);
	}
	while (length > 0) {
		if (*pointer1 != *pointer2) {
			return false;
		}
		pointer1++;
		pointer2++;
		length--;
	}
	return true;
}

/**
 * Verify the Albireo-provided deduplication advice, and invoke a
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

	data_vio_add_trace_record(data_vio,
				   THIS_LOCATION("$F;j=dedupe;cb=verify"));
	if (likely(memory_equal(data_vio->data_block,
				data_vio->read_block.data,
				VDO_BLOCK_SIZE))) {
		// Leave data_vio->is_duplicate set to true.
	} else {
		data_vio->is_duplicate = false;
	}

	kvdo_enqueue_data_vio_callback(data_vio);
}

/**
 * Verify the Albireo-provided deduplication advice, and invoke a
 * callback once the answer is available.
 *
 * @param completion  The data_vio that we are looking to dedupe.
 **/
static void verify_read_block_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	int err = data_vio->read_block.status;

	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	if (unlikely(err != 0)) {
		log_debug("%s: err %d", __func__, err);
		data_vio->is_duplicate = false;
		kvdo_enqueue_data_vio_callback(data_vio);
		return;
	}

	launch_data_vio_on_cpu_queue(data_vio,
				     verify_duplication_work,
				     NULL,
				     CPU_Q_ACTION_COMPRESS_BLOCK);
}

/**********************************************************************/
void verify_duplication(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(data_vio->is_duplicate,
			"advice to verify must be valid");
	ASSERT_LOG_ONLY(data_vio->duplicate.state != MAPPING_STATE_UNMAPPED,
			"advice to verify must not be a discard");
	ASSERT_LOG_ONLY(data_vio->duplicate.pbn != ZERO_BLOCK,
			"advice to verify must not point to the zero block");
	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"zeroed block should not have advice to verify");

	const struct trace_location *location =
		THIS_LOCATION("verifyDuplication;dup=update(verify);io=verify");
	data_vio_add_trace_record(data_vio, location);
	kvdo_read_block(data_vio,
			data_vio->duplicate.pbn,
			data_vio->duplicate.state,
			BIO_Q_ACTION_VERIFY,
			verify_read_block_callback);
}

/**********************************************************************/
bool compare_data_vios(struct data_vio *first, struct data_vio *second)
{
	data_vio_add_trace_record(second, THIS_LOCATION(NULL));
	return memory_equal(first->data_block, second->data_block,
			    VDO_BLOCK_SIZE);
}
