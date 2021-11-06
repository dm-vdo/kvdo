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
 */

#ifndef DEDUPE_INDEX_H
#define DEDUPE_INDEX_H

#include "uds.h"

#include "dataKVIO.h"
#include "types.h"

int __must_check
make_vdo_dedupe_index(struct dedupe_index **index_ptr,
		      struct vdo *vdo,
		      const char *thread_name_prefix);

void dump_vdo_dedupe_index(struct dedupe_index *index);

void free_vdo_dedupe_index(struct dedupe_index *index);

const char *get_vdo_dedupe_index_state_name(struct dedupe_index *index);

uint64_t get_vdo_dedupe_index_timeout_count(struct dedupe_index *index);

void get_vdo_dedupe_index_statistics(struct dedupe_index *index,
				     struct index_statistics *stats);

int message_vdo_dedupe_index(struct dedupe_index *index, const char *name);

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

int add_vdo_dedupe_index_sysfs(struct dedupe_index *index,
			       struct kobject *parent);

void start_vdo_dedupe_index(struct dedupe_index *index, bool create_flag);

void suspend_vdo_dedupe_index(struct dedupe_index *index, bool save_flag);

void resume_vdo_dedupe_index(struct dedupe_index *index,
			     bool dedupe,
			     bool create);

void finish_vdo_dedupe_index(struct dedupe_index *index);

/*
 * Interval (in milliseconds or jiffies) from submission until switching to 
 * fast path and skipping UDS. 
 */
extern unsigned int vdo_dedupe_index_timeout_interval;

/*
 * Minimum time interval (in milliseconds) between timer invocations to 
 * check for requests waiting for UDS that should now time out. 
 */
extern unsigned int vdo_dedupe_index_min_timer_interval;

void set_vdo_dedupe_index_timeout_interval(unsigned int value);

void set_vdo_dedupe_index_min_timer_interval(unsigned int value);

#endif /* DEDUPE_INDEX_H */
