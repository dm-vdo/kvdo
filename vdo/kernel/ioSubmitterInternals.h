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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/ioSubmitterInternals.h#1 $
 */

#ifndef IOSUBMITTERINTERNALS_H
#define IOSUBMITTERINTERNALS_H

#include <linux/version.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,38)
#include <linux/blkdev.h>
#endif

#include "ioSubmitter.h"
#include "readCache.h"

/**
 * Various strategies for assigning read-cache and bio-submission work
 * to various threads.
 *
 * At the moment, several possibilities are available. Read cache
 * operations will always be performed in a bio submission thread
 * associated with the read cache zone determined by some physical
 * block number, or in batches in a "cpu" thread. The actual bio
 * submission operations can either be submitted from the same bio
 * submission thread, or they can be redistributed across the threads
 * round-robin.
 **/
enum ioWorkStrategy {
  /*
   * Read cache and bio submission operations all go to bio submission
   * threads chosen based on the PBN. Bio-submission-only operations
   * are assigned round-robin.
   *
   * Read cache operations may get queued up behind potentially
   * blocking I/O operations in the same work queue. The assignment of
   * I/O operations to work queues by PBN can result in imbalanced
   * assignments and less efficient use of the bio threads. However,
   * we avoid the cost of the additional thread switch that
   * IWS_RC_PBN_BIO_RR incurs.
   */
  IWS_RC_PBN_BIO_PBN,
  /*
   * Read cache operations go to bio submission threads chosen based
   * on PBN; bio submission operations are re-queued for the bio
   * submission threads in round-robin style.
   *
   * Read cache operations may get queued up behind potentially
   * blocking I/O operations in the same work queue. Use of the bio
   * threads for I/O should be evenly distributed and rotating among
   * the threads. In testing, at least some of the benefit seems to
   * come from each bio thread essentially switching modes between
   * accumulating I/Os in the blk_plug and passing them off to the
   * device (unplugging).
   */
  IWS_RC_PBN_BIO_RR,
  /*
   * Read cache operations are processed in batches (via a
   * BatchProcessor in the zone structure) in the CPU threads; bio
   * submission operations are assigned to the bio submission threads
   * in round-robin style. There is always exactly one zone.
   *
   * This avoids read-cache operations getting stuck behind
   * potentially blocking I/O operations, but there could be hashing
   * operations queued up first. Most operations, if they find nothing
   * in the cache or simply want to invalidate a cache entry, will
   * need to do an I/O operation anyway. This includes all writes. So
   * the gain is limited. However, the "release" operation doesn't
   * need the bio work queue, nor does any read operation finding a
   * hit in the cache.
   */
  IWS_RC_BATCH_BIO_RR,

  // Some possible future experiments:
  /*
   * Read cache operations are processed in the calling thread, with
   * locking contention reduced via using lots of zones; bio
   * submission operations are assigned to the bio submission threads
   * in round-robin style.
   */
  //IWS_RC_CALLER_BIO_RR,
  /*
   * Read cache operations get their own threads (one per zone). Bio
   * submission operations are assigned to bio submission threads in
   * round-robin style.
   */
  //IWS_RC_OWN_BIO_RR,

  // The actual work scheduling strategy we're compiling for.
  IO_WORK_STRATEGY = IWS_RC_PBN_BIO_PBN,
};

/*
 * Submission of bio operations to the underlying storage device will
 * go through a separate work queue thread (or more than one) to
 * prevent blocking in other threads if the storage device has a full
 * queue. The plug structure allows that thread to do better batching
 * of requests to make the I/O more efficient.
 *
 * The map (protected by the mutex) collects pending I/O operations so
 * that the worker thread can reorder them to try to encourage I/O
 * request merging in the request queue underneath.
 *
 * At least, that's the general idea. Actual usage of the various
 * fields may depend on how options are configured in ioSubmitter.c.
 */
typedef struct bioQueueData {
  KvdoWorkQueue         *queue;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,38)
  struct blk_plug        plug;
#else
  struct block_device   *bdev;
#endif
  IntMap                *map;
  struct mutex           lock;
  unsigned int           queueNumber;
} BioQueueData;

struct ioSubmitter {
  unsigned int     numBioQueuesUsed;
  unsigned int     bioQueueRotationInterval;
  unsigned int     bioQueueRotor;
  ReadCache       *readCache;
  BioQueueData     bioQueueData[];
};

/**
 * Returns the BioQueueData pointer associated with the current thread.
 * Results are undefined if called from any other thread.
 *
 * @return the BioQueueData pointer
 **/
static inline BioQueueData *getCurrentBioQueueData(void)
{
  BioQueueData *bioQueueData = (BioQueueData *) getWorkQueuePrivateData();
  // Does it look like a bio queue thread?
  BUG_ON(bioQueueData == NULL);
  BUG_ON(bioQueueData->queue != getCurrentWorkQueue());
  return bioQueueData;
}

/**
 * Update stats and tracing info, then submit the supplied bio to the
 * OS for processing.
 *
 * @param kvio      The KVIO associated with the bio
 * @param bio       The bio to submit to the OS
 * @param location  Call site location for tracing
 **/
void sendBioToDevice(KVIO *kvio, BIO *bio, TraceLocation location);

/**
 * Return the bio thread number handling the specified physical block
 * number.
 *
 * @param ioSubmitter       The I/O submitter data
 * @param pbn               The physical block number
 *
 * @return read cache zone number
 **/
unsigned int bioQueueNumberForPBN(IOSubmitter         *ioSubmitter,
                                  PhysicalBlockNumber  pbn);

/**
 * Enqueue a work item to run in the work queue associated with the specified
 * pbn used for bio submissions from the physical layer.
 *
 * @param ioSubmitter       The I/O submitter data to update
 * @param pbn               The physical block number
 * @param workItem          The new work item to run
 **/
void enqueueByPBNBioWorkItem(IOSubmitter         *ioSubmitter,
                             PhysicalBlockNumber  pbn,
                             KvdoWorkItem        *workItem);

/**
 * Enqueue an operation to run in a bio submission thread appropriate
 * to the indicated physical block number, possibly reordered via the
 * "bio map" to improve sequential access patterns.
 *
 * The work item in the KVIO at bio->bi_private is what will be
 * enqueued.
 *
 * @param bio       The bio to eventually be submitted
 * @param action    The work queue action code to prioritize processing
 * @param callback  The function to invoke in the work queue thread
 * @param pbn       The physical block number that may be accessed
 **/
void enqueueBioMap(BIO                 *bio,
                   BioQAction           action,
                   KvdoWorkFunction     callback,
                   PhysicalBlockNumber  pbn);

/**
 * Check that we're running normally (i.e., not in an
 * interrupt-servicing context) in an IOSubmitter bio thread.
 **/
void assertRunningInBioQueue(void);

/**
 * Check that we're running normally (i.e., not in an
 * interrupt-servicing context) in an IOSubmitter bio thread. Also
 * require that the thread we're running on is the correct one for the
 * supplied physical block number.
 *
 * @param pbn  The PBN that should have been used in thread selection
 **/
void assertRunningInBioQueueForPBN(PhysicalBlockNumber pbn);

#endif /* IOSUBMITTERINTERNALS_H */
