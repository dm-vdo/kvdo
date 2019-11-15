/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dedupeIndex.h#14 $
 */

#ifndef DEDUPE_INDEX_H
#define DEDUPE_INDEX_H

#include "dataKVIO.h"

/**
 * Make a dedupe index
 *
 * @param index_ptr  dedupe index returned here
 * @param layer      the kernel layer
 *
 * @return VDO_SUCCESS or an error code
 **/
int make_dedupe_index(struct dedupe_index **index_ptr,
		      struct kernel_layer *layer)
	__attribute__((warn_unused_result));


/**
 * Do the dedupe section of dmsetup message vdo0 0 dump ...
 *
 * @param index       The dedupe index
 * @param show_queue  true to dump a dedupe work queue
 **/
void dump_dedupe_index(struct dedupe_index *index, bool show_queue);

/**
 * Free the dedupe index
 *
 * @param index_ptr  The dedupe index
 **/
void free_dedupe_index(struct dedupe_index **index_ptr);

/**
 * Get the name of the deduplication state
 *
 * @param index  The dedupe index
 *
 * @return the dedupe state name
 **/
const char *get_dedupe_state_name(struct dedupe_index *index);

/**
 * Get the dedupe timeout count.
 *
 * @param index  The dedupe index
 *
 * @return The number of dedupe timeouts noted
 **/
uint64_t get_dedupe_timeout_count(struct dedupe_index *index);

/**
 * Get the index statistics
 *
 * @param index  The dedupe index
 * @param stats  The index statistics
 **/
void get_index_statistics(struct dedupe_index *index, IndexStatistics *stats);

/**
 * Return from a dedupe operation by invoking the callback function
 *
 * @param data_kvio  The data_kvio
 **/
static inline void invoke_dedupe_callback(struct data_kvio *data_kvio)
{

	data_kvio_add_trace_record(data_kvio,
				   THIS_LOCATION("$F($dup);cb=dedupe($dup)"));
	kvdo_enqueue_data_vio_callback(data_kvio);
}

/**
 * Process a dmsetup message directed to the index.
 *
 * @param index  The dedupe index
 * @param name   The message name
 *
 * @return 0 or an error code
 **/
int message_dedupe_index(struct dedupe_index *index, const char *name);

/**
 * Look up the chunkname of the data_kvio and identify duplicated chunks.
 *
 * @param data_kvio  The data_kvio. These fields are used:
 *                   dedupe_context.chunkName is the chunk name.
 *                   The advice to offer to the index will be obtained
 *                   via get_dedupe_advice(). The advice found in the index
 *                   (or NULL if none) will be returned via
 *                   set_dedupe_advice(). dedupe_context.status is set to the
 *                   return status code of any asynchronous index processing.
 **/
void post_dedupe_advice(struct data_kvio *data_kvio);

/**
 * Look up the chunkname of the data_kvio and identify duplicated chunks.
 *
 * @param data_kvio  The data_kvio. These fields are used:
 *                   dedupe_context.chunkName is the chunk name.
 *                   The advice found in the index (or NULL if none) will
 *                   be returned via set_dedupe_advice().
 *                   dedupe_context.status is set to the return status code of
 *                   any asynchronous index processing.
 **/
void query_dedupe_advice(struct data_kvio *data_kvio);

/**
 * Start the dedupe index.
 *
 * @param index        The dedupe index
 * @param create_flag  If true, create a new index without first attempting
 *                     to load an existing index
 **/
void start_dedupe_index(struct dedupe_index *index, bool create_flag);

/**
 * Stop the dedupe index.  May be called by any thread, but will wait for
 * the shutdown to be completed.
 *
 * @param index  The dedupe index
 **/
void stop_dedupe_index(struct dedupe_index *index);

/**
 * Wait until the dedupe index has completed all its outstanding I/O. 
 * May be called from any thread,
 *
 * @param index      The dedupe index
 * @param save_flag  True if we should save the index
 **/
void suspend_dedupe_index(struct dedupe_index *index, bool save_flag);

/**
 * Resume a suspended dedupe index. May be called from any thread.
 *
 * @param index  The dedupe index
 **/
void resume_dedupe_index(struct dedupe_index *index);

/**
 * Finish the dedupe index.
 *
 * @param index  The dedupe index
 **/
void finish_dedupe_index(struct dedupe_index *index);

/**
 * Look up the chunkname of the data_kvio and associate the new PBN with the
 * name.
 *
 * @param data_kvio  The data_kvio. These fields are used:
 *                   dedupe_context.chunkName is the chunk name.
 *                   The advice to offer to the index will be obtained via
 *                   get_dedupe_advice(). dedupe_context.status is set to the
 *                   return status code of any asynchronous index processing.
 **/
void update_dedupe_advice(struct data_kvio *data_kvio);

// Interval (in milliseconds or jiffies) from submission until switching to
// fast path and skipping Albireo.
extern unsigned int albireo_timeout_interval;

// Minimum time interval (in milliseconds) between timer invocations to
// check for requests waiting for Albireo that should now time out.
extern unsigned int min_albireo_timer_interval;

/**
 * Set the interval from submission until switching to fast path and
 * skipping Albireo.
 *
 * @param value  The number of milliseconds
 **/
void set_albireo_timeout_interval(unsigned int value);

/**
 * Set the minimum time interval between timer invocations to check for
 * requests waiting for Albireo that should now time out.
 *
 * @param value  The number of milliseconds
 **/
void set_min_albireo_timer_interval(unsigned int value);

#endif /* DEDUPE_INDEX_H */
