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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dedupeIndex.h#1 $
 */

#ifndef DEDUPE_INDEX_H
#define DEDUPE_INDEX_H

#include "dataKVIO.h"

struct dedupeIndex {

  /**
   * Do the dedupe section of dmsetup message vdo0 0 dump ...
   *
   * @param index      The dedupe index
   * @param showQueue  true to dump a dedupe work queue
   **/
  void (*dump)(DedupeIndex *index, bool showQueue);

  /**
   * Free a dedupe index. The "finish" method must have been called
   * first.
   *
   * @param index  The dedupe index
   **/
  void (*free)(DedupeIndex *index);

  /**
   * Get the name of the deduplication state
   *
   * @param index  The dedupe index
   *
   * @return the dedupe state name
   **/
  const char *(*getDedupeStateName)(DedupeIndex *index);

  /**
   * Get the index statistics
   *
   * @param index  The dedupe index
   * @param stats  The index statistics
   **/
  void (*getStatistics)(DedupeIndex *index, IndexStatistics *stats);

  /**
   * Process a dmsetup message directed to the index.
   *
   * @param index  The dedupe index
   * @param name   The message name
   *
   * @return 0 or an error code
   **/
  int (*message)(DedupeIndex *index, const char *name);

  /**
   * Look up the chunkname of the DataKVIO. If found, return the PBN
   * previously associated with the name. If not found, associate the
   * new PBN with the name.
   *
   * @param dataKVIO  The DataKVIO
   **/
  void (*post)(DataKVIO *dataKVIO);

  /**
   * Look up the chunkname of the DataKVIO. If found, return the PBN
   * previously associated with the name. If not found, do nothing.
   *
   * @param dataKVIO  The DataKVIO
   **/
  void (*query)(DataKVIO *dataKVIO);

  /**
   * Start the dedupe index.
   *
   * @param index       The dedupe index
   * @param createFlag  If true, create a new index without first attempting
   *                    to load an existing index
   **/
  void (*start)(DedupeIndex *index, bool createFlag);

  /**
   * Stop the dedupe index.  May be called by any thread, but will wait for
   * the shutdown to be completed.
   *
   * @param index  The dedupe index
   **/
  void (*stop)(DedupeIndex *index);

  /**
   * Finish the dedupe index; shuts it down for good and prepares to
   * free resources. After this point, no more requests may be sent to
   * it.
   *
   * @param index   The dedupe index
   **/
  void (*finish)(DedupeIndex *index);

  /**
   * Look up the chunkname of the DataKVIO and associate the new PBN with the
   * name.
   *
   * @param dataKVIO  The DataKVIO
   **/
  void (*update)(DataKVIO *dataKVIO);
};

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
 * @param index  The dedupe index
 * @param showQueue  true to dump a dedupe work queue
 **/
static inline void dumpDedupeIndex(DedupeIndex *index, bool showQueue)
{
  index->dump(index, showQueue);
}

/**
 * Free the dedupe index
 *
 * @param index  The dedupe index
 **/
static inline void freeDedupeIndex(DedupeIndex **index)
{
  if (*index != NULL) {
    (*index)->free(*index);
    *index = NULL;
  }
}

/**
 * Get the name of the deduplication state
 *
 * @param index  The dedupe index
 *
 * @return the dedupe state name
 **/
static inline const char *getDedupeStateName(DedupeIndex *index)
{
  return index->getDedupeStateName(index);
}

/**
 * Get the index statistics
 *
 * @param index  The dedupe index
 * @param stats  The index statistics
 **/
static inline void getIndexStatistics(DedupeIndex     *index,
                                      IndexStatistics *stats)
{
  return index->getStatistics(index, stats);
}

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
static inline int messageDedupeIndex(DedupeIndex *index, const char *name)
{
  return index->message(index, name);
}

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
static inline void postDedupeAdvice(DataKVIO *dataKVIO)
{
  KernelLayer *layer = dataKVIOAsKVIO(dataKVIO)->layer;
  layer->dedupeIndex->post(dataKVIO);
}

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
static inline void queryDedupeAdvice(DataKVIO *dataKVIO)
{
  KernelLayer *layer = dataKVIOAsKVIO(dataKVIO)->layer;
  layer->dedupeIndex->query(dataKVIO);
}

/**
 * Start the dedupe index.
 *
 * @param index       The dedupe index
 * @param createFlag  If true, create a new index without first attempting
 *                    to load an existing index
 **/
static inline void startDedupeIndex(DedupeIndex *index, bool createFlag)
{
  index->start(index, createFlag);
}

/**
 * Stop the dedupe index.  May be called by any thread, but will wait for
 * the shutdown to be completed.
 *
 * @param index  The dedupe index
 **/
static inline void stopDedupeIndex(DedupeIndex *index)
{
  return index->stop(index);
}

/**
 * Finish the dedupe index.
 *
 * @param index  The dedupe index
 **/
static inline void finishDedupeIndex(DedupeIndex *index)
{
  return index->finish(index);
}

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
static inline void updateDedupeAdvice(DataKVIO *dataKVIO)
{
  KernelLayer *layer = dataKVIOAsKVIO(dataKVIO)->layer;
  layer->dedupeIndex->update(dataKVIO);
}

// Interval (in milliseconds or jiffies) from submission until switching to
// fast path and skipping Albireo.
extern unsigned int albireoTimeoutInterval;
extern Jiffies      albireoTimeoutJiffies;

// Minimum time interval (in milliseconds) between timer invocations to
// check for requests waiting for Albireo that should now time out.
extern unsigned int minAlbireoTimerInterval;

/**
 * Calculate the actual end of a timer, taking into account the absolute
 * start time and the present time.
 *
 * @param startJiffies  The absolute start time, in jiffies
 *
 * @return the absolute end time for the timer, in jiffies
 **/
Jiffies getAlbireoTimeout(Jiffies startJiffies);

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
