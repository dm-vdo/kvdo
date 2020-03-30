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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/verify.c#14 $
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
__attribute__((warn_unused_result))
static bool
memory_equal(void *pointer_argument1,
	     void *pointer_argument2,
	     size_t length)
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
static void verify_duplication_work(struct kvdo_work_item *item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(item);

	data_kvio_add_trace_record(data_kvio,
				   THIS_LOCATION("$F;j=dedupe;cb=verify"));

	if (likely(memory_equal(data_kvio->data_block,
				data_kvio->read_block.data,
				VDO_BLOCK_SIZE))) {
		// Leave data_kvio->data_vio.isDuplicate set to true.
	} else {
		data_kvio->data_vio.isDuplicate = false;
	}

	kvdo_enqueue_data_vio_callback(data_kvio);
}

/**
 * Verify the Albireo-provided deduplication advice, and invoke a
 * callback once the answer is available.
 *
 * @param data_kvio  The data_kvio that we are looking to dedupe.
 **/
static void verify_read_block_callback(struct data_kvio *data_kvio)
{
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));
	int err = data_kvio->read_block.status;

	if (unlikely(err != 0)) {
		logDebug("%s: err %d", __func__, err);
		data_kvio->data_vio.isDuplicate = false;
		kvdo_enqueue_data_vio_callback(data_kvio);
		return;
	}

	launch_data_kvio_on_cpu_queue(data_kvio,
				      verify_duplication_work,
				      NULL,
				      CPU_Q_ACTION_COMPRESS_BLOCK);
}

/**********************************************************************/
void verifyDuplication(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(data_vio->isDuplicate,
			"advice to verify must be valid");
	ASSERT_LOG_ONLY(data_vio->duplicate.state != MAPPING_STATE_UNMAPPED,
			"advice to verify must not be a discard");
	ASSERT_LOG_ONLY(data_vio->duplicate.pbn != ZERO_BLOCK,
			"advice to verify must not point to the zero block");
	ASSERT_LOG_ONLY(!data_vio->isZeroBlock,
			"zeroed block should not have advice to verify");

	TraceLocation *location =
		THIS_LOCATION("verifyDuplication;dup=update(verify);io=verify");
	data_vio_add_trace_record(data_vio, location);
	kvdo_read_block(data_vio,
			data_vio->duplicate.pbn,
			data_vio->duplicate.state,
			BIO_Q_ACTION_VERIFY,
			verify_read_block_callback);
}

/**********************************************************************/
bool compareDataVIOs(struct data_vio *first, struct data_vio *second)
{
	data_vio_add_trace_record(second, THIS_LOCATION(NULL));
	struct data_kvio *a = data_vio_as_data_kvio(first);
	struct data_kvio *b = data_vio_as_data_kvio(second);

	return memory_equal(a->data_block, b->data_block, VDO_BLOCK_SIZE);
}
