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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dedupeIndex.h#37 $
 */

#ifndef DEDUPE_INDEX_H
#define DEDUPE_INDEX_H

#include "uds.h"

#include "dataKVIO.h"
#include "types.h"

/**
 * Make a dedupe index
 *
 * @param index_ptr           dedupe index returned here
 * @param vdo                 the vdo to which the index will belong
 * @param thread_name_prefix  The per-device prefix to use in thread names
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
make_vdo_dedupe_index(struct dedupe_index **index_ptr,
		      struct vdo *vdo,
		      const char *thread_name_prefix);

/**
 * Do the dedupe section of dmsetup message vdo0 0 dump ...
 *
 * @param index       The dedupe index
 **/
void dump_vdo_dedupe_index(struct dedupe_index *index);

/**
 * Free the dedupe index
 *
 * @param index  The dedupe index
 **/
void free_vdo_dedupe_index(struct dedupe_index *index);

/**
 * Get the name of the deduplication state
 *
 * @param index  The dedupe index
 *
 * @return the dedupe state name
 **/
const char *get_vdo_dedupe_index_state_name(struct dedupe_index *index);

/**
 * Get the dedupe timeout count.
 *
 * @param index  The dedupe index
 *
 * @return The number of dedupe timeouts noted
 **/
uint64_t get_vdo_dedupe_index_timeout_count(struct dedupe_index *index);

/**
 * Get the index statistics
 *
 * @param index  The dedupe index
 * @param stats  The index statistics
 **/
void get_vdo_dedupe_index_statistics(struct dedupe_index *index,
				     struct index_statistics *stats);

/**
 * Process a dmsetup message directed to the index.
 *
 * @param index  The dedupe index
 * @param name   The message name
 *
 * @return 0 or an error code
 **/
int message_vdo_dedupe_index(struct dedupe_index *index, const char *name);

/**
 * Enqueue operation for submission to the index.
 *
 * @param data_vio   The data_vio requesting the operation
 * @param operation  The index operation to perform
 **/
void enqueue_vdo_index_operation(struct data_vio *data_vio,
				 enum uds_request_type operation);

/**
 * Look up the chunkname of the data_vio and identify duplicated chunks.
 *
 * @param data_vio  The data_vio. These fields are used:
 *                  data_vio.chunk_name is the chunk name. The advice to
 *                  offer to the index will be obtained via
 *                  vdo_get_dedupe_advice(). The advice found in the index (or
 *                  NULL if none) will be returned via vdo_set_dedupe_advice().
 *                  dedupe_context.status is set to the return status code of
 *                  any asynchronous index processing.
 **/
static inline void post_vdo_dedupe_advice(struct data_vio *data_vio)
{
	enqueue_vdo_index_operation(data_vio, UDS_POST);
}

/**
 * Look up the chunk_name of the data_vio and identify duplicated chunks.
 *
 * @param data_vio  The data_vio. These fields are used:
 *                  data_vio.chunk_name is the chunk name. The advice
 *                  found in the index (or NULL if none) will be returned via
 *                  vdo_set_dedupe_advice(). dedupe_context.status is set to
 *                  the return status code of any asynchronous index
 *                  processing.
 **/
static inline void query_vdo_dedupe_advice(struct data_vio *data_vio)
{
	enqueue_vdo_index_operation(data_vio, UDS_QUERY);
}

/**
 * Look up the chunk_name of the data_vio and associate the new PBN with the
 * name.
 *
 * @param data_vio  The data_vio. These fields are used:
 *                  data_vio.chunk_name is the chunk name. The advice to
 *                  offer to the index will be obtained via
 *                  vdo_get_dedupe_advice(). dedupe_context.status is set to
 *                  the return status code of any asynchronous index
 *                  processing.
 **/
static inline void update_vdo_dedupe_advice(struct data_vio *data_vio)
{
	enqueue_vdo_index_operation(data_vio, UDS_UPDATE);
}

/**
 * Add the sysfs nodes for the dedupe index.
 *
 * @param index        The dedupe index
 * @param parent  The kobject to attach the sysfs nodes to
 *
 * @return 0 or an error code
 **/
int add_vdo_dedupe_index_sysfs(struct dedupe_index *index,
			       struct kobject *parent);

/**
 * Start the dedupe index.
 *
 * @param index        The dedupe index
 * @param create_flag  If true, create a new index without first attempting
 *                     to load an existing index
 **/
void start_vdo_dedupe_index(struct dedupe_index *index, bool create_flag);

/**
 * Wait until the dedupe index has completed all its outstanding I/O.
 * May be called from any thread,
 *
 * @param index      The dedupe index
 * @param save_flag  True if we should save the index
 **/
void suspend_vdo_dedupe_index(struct dedupe_index *index, bool save_flag);

/**
 * Resume a suspended dedupe index. May be called from any thread.
 *
 * @param index   The dedupe index
 * @param dedupe  Whether dedupe should be on or off.
 * @param create  Whether to create the index or not.
 **/
void resume_vdo_dedupe_index(struct dedupe_index *index,
			     bool dedupe,
			     bool create);

/**
 * Finish the dedupe index.
 *
 * @param index  The dedupe index
 **/
void finish_vdo_dedupe_index(struct dedupe_index *index);

// Interval (in milliseconds or jiffies) from submission until switching to
// fast path and skipping UDS.
extern unsigned int vdo_dedupe_index_timeout_interval;

// Minimum time interval (in milliseconds) between timer invocations to
// check for requests waiting for UDS that should now time out.
extern unsigned int vdo_dedupe_index_min_timer_interval;

/**
 * Set the interval from submission until switching to fast path and
 * skipping UDS.
 *
 * @param value  The number of milliseconds
 **/
void set_vdo_dedupe_index_timeout_interval(unsigned int value);

/**
 * Set the minimum time interval between timer invocations to check for
 * requests waiting for UDS that should now time out.
 *
 * @param value  The number of milliseconds
 **/
void set_vdo_dedupe_index_min_timer_interval(unsigned int value);

#endif /* DEDUPE_INDEX_H */
