/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dedupeIndex.h#8 $
 */

#ifndef DEDUPE_INDEX_H
#define DEDUPE_INDEX_H

#include "dataKVIO.h"

/**
 * Make a dedupe index
 *
 * @param indexPtr  dedupe index returned here
 * @param layer     the kernel layer
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeDedupeIndex(struct dedupe_index **indexPtr, KernelLayer *layer)
  __attribute__((warn_unused_result));


/**
 * Do the dedupe section of dmsetup message vdo0 0 dump ...
 *
 * @param index      The dedupe index
 * @param showQueue  true to dump a dedupe work queue
 **/
void dumpDedupeIndex(struct dedupe_index *index, bool showQueue);

/**
 * Free the dedupe index
 *
 * @param indexPtr  The dedupe index
 **/
void freeDedupeIndex(struct dedupe_index **indexPtr);

/**
 * Get the name of the deduplication state
 *
 * @param index  The dedupe index
 *
 * @return the dedupe state name
 **/
const char *getDedupeStateName(struct dedupe_index *index);

/**
 * Get the dedupe timeout count.
 *
 * @param index  The dedupe index
 *
 * @return The number of dedupe timeouts noted
 **/
uint64_t getDedupeTimeoutCount(struct dedupe_index *index);

/**
 * Get the index statistics
 *
 * @param index  The dedupe index
 * @param stats  The index statistics
 **/
void getIndexStatistics(struct dedupe_index *index, IndexStatistics *stats);

/**
 * Return from a dedupe operation by invoking the callback function
 *
 * @param dataKVIO  The data_kvio
 **/
static inline void invokeDedupeCallback(struct data_kvio *dataKVIO)
{

  data_kvio_add_trace_record(dataKVIO,
                             THIS_LOCATION("$F($dup);cb=dedupe($dup)"));
  kvdo_enqueue_data_vio_callback(dataKVIO);
}

/**
 * Process a dmsetup message directed to the index.
 *
 * @param index  The dedupe index
 * @param name   The message name
 *
 * @return 0 or an error code
 **/
int messageDedupeIndex(struct dedupe_index *index, const char *name);

/**
 * Look up the chunkname of the data_kvio and identify duplicated chunks.
 *
 * @param dataKVIO  The data_kvio. These fields are used:
 *                  dedupeContext.chunkName is the chunk name.
 *                  The advice to offer to the index will be obtained
 *                  via getDedupeAdvice(). The advice found in the index
 *                  (or NULL if none) will be returned via setDedupeAdvice().
 *                  dedupeContext.status is set to the return status code of
 *                  any asynchronous index processing.
 **/
void postDedupeAdvice(struct data_kvio *dataKVIO);

/**
 * Look up the chunkname of the data_kvio and identify duplicated chunks.
 *
 * @param dataKVIO  The data_kvio. These fields are used:
 *                  dedupeContext.chunkName is the chunk name.
 *                  The advice found in the index (or NULL if none) will
 *                  be returned via setDedupeAdvice().
 *                  dedupeContext.status is set to the return status code of
 *                  any asynchronous index processing.
 **/
void queryDedupeAdvice(struct data_kvio *dataKVIO);

/**
 * Start the dedupe index.
 *
 * @param index       The dedupe index
 * @param createFlag  If true, create a new index without first attempting
 *                    to load an existing index
 **/
void startDedupeIndex(struct dedupe_index *index, bool createFlag);

/**
 * Stop the dedupe index.  May be called by any thread, but will wait for
 * the shutdown to be completed.
 *
 * @param index  The dedupe index
 **/
void stopDedupeIndex(struct dedupe_index *index);

/**
 * Wait until the dedupe index has completed all its outstanding I/O.
 *
 * @param index     The dedupe index
 * @param saveFlag  True if we should save the index
 **/
void suspendDedupeIndex(struct dedupe_index *index, bool saveFlag);

/**
 * Finish the dedupe index.
 *
 * @param index  The dedupe index
 **/
void finishDedupeIndex(struct dedupe_index *index);

/**
 * Look up the chunkname of the data_kvio and associate the new PBN with the
 * name.
 *
 * @param dataKVIO  The data_kvio. These fields are used:
 *                  dedupeContext.chunkName is the chunk name.
 *                  The advice to offer to the index will be obtained
 *                  via getDedupeAdvice(). dedupeContext.status is set to the
 *                  return status code of any asynchronous index processing.
 **/
void updateDedupeAdvice(struct data_kvio *dataKVIO);

// Interval (in milliseconds or jiffies) from submission until switching to
// fast path and skipping Albireo.
extern unsigned int albireoTimeoutInterval;

// Minimum time interval (in milliseconds) between timer invocations to
// check for requests waiting for Albireo that should now time out.
extern unsigned int minAlbireoTimerInterval;

/**
 * Set the interval from submission until switching to fast path and
 * skipping Albireo.
 *
 * @param value  The number of milliseconds
 **/
void setAlbireoTimeoutInterval(unsigned int value);

/**
 * Set the minimum time interval between timer invocations to check for
 * requests waiting for Albireo that should now time out.
 *
 * @param value  The number of milliseconds
 **/
void setMinAlbireoTimerInterval(unsigned int value);

#endif /* DEDUPE_INDEX_H */
