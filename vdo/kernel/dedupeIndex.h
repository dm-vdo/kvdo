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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dedupeIndex.h#3 $
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
int makeDedupeIndex(DedupeIndex **indexPtr, KernelLayer *layer)
  __attribute__((warn_unused_result));


/**
 * Do the dedupe section of dmsetup message vdo0 0 dump ...
 *
 * @param index      The dedupe index
 * @param showQueue  true to dump a dedupe work queue
 **/
void dumpDedupeIndex(DedupeIndex *index, bool showQueue);

/**
 * Free the dedupe index
 *
 * @param indexPtr  The dedupe index
 **/
void freeDedupeIndex(DedupeIndex **indexPtr);

/**
 * Get the name of the deduplication state
 *
 * @param index  The dedupe index
 *
 * @return the dedupe state name
 **/
const char *getDedupeStateName(DedupeIndex *index);

/**
 * Get the index statistics
 *
 * @param index  The dedupe index
 * @param stats  The index statistics
 **/
void getIndexStatistics(DedupeIndex *index, IndexStatistics *stats);

/**
 * Return from a dedupe operation by invoking the callback function
 *
 * @param dataKVIO  The DataKVIO
 **/
static inline void invokeDedupeCallback(DataKVIO *dataKVIO)
{

  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION("$F($dup);cb=dedupe($dup)"));
  kvdoEnqueueDataVIOCallback(dataKVIO);
}

/**
 * Process a dmsetup message directed to the index.
 *
 * @param index  The dedupe index
 * @param name   The message name
 *
 * @return 0 or an error code
 **/
int messageDedupeIndex(DedupeIndex *index, const char *name);

/**
 * Look up the chunkname of the DataKVIO and identify duplicated chunks.
 *
 * @param dataKVIO  The DataKVIO. These fields are used:
 *                  dedupeContext.chunkName is the chunk name.
 *                  The advice to offer to the index will be obtained
 *                  via getDedupeAdvice(). The advice found in the index
 *                  (or NULL if none) will be returned via setDedupeAdvice().
 *                  dedupeContext.status is set to the return status code of
 *                  any asynchronous index processing.
 **/
void postDedupeAdvice(DataKVIO *dataKVIO);

/**
 * Look up the chunkname of the DataKVIO and identify duplicated chunks.
 *
 * @param dataKVIO  The DataKVIO. These fields are used:
 *                  dedupeContext.chunkName is the chunk name.
 *                  The advice found in the index (or NULL if none) will
 *                  be returned via setDedupeAdvice().
 *                  dedupeContext.status is set to the return status code of
 *                  any asynchronous index processing.
 **/
void queryDedupeAdvice(DataKVIO *dataKVIO);

/**
 * Start the dedupe index.
 *
 * @param index       The dedupe index
 * @param createFlag  If true, create a new index without first attempting
 *                    to load an existing index
 **/
void startDedupeIndex(DedupeIndex *index, bool createFlag);

/**
 * Stop the dedupe index.  May be called by any thread, but will wait for
 * the shutdown to be completed.
 *
 * @param index  The dedupe index
 **/
void stopDedupeIndex(DedupeIndex *index);

/**
 * Finish the dedupe index.
 *
 * @param index  The dedupe index
 **/
void finishDedupeIndex(DedupeIndex *index);

/**
 * Look up the chunkname of the DataKVIO and associate the new PBN with the
 * name.
 *
 * @param dataKVIO  The DataKVIO. These fields are used:
 *                  dedupeContext.chunkName is the chunk name.
 *                  The advice to offer to the index will be obtained
 *                  via getDedupeAdvice(). dedupeContext.status is set to the
 *                  return status code of any asynchronous index processing.
 **/
void updateDedupeAdvice(DataKVIO *dataKVIO);

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
