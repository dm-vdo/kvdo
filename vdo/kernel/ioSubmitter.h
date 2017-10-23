/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/ioSubmitter.h#1 $
 */

#ifndef IOSUBMITTER_H
#define IOSUBMITTER_H

#include <linux/version.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,38)
#include <linux/blkdev.h>
#endif

#include "kernelLayer.h"
#include "kvio.h"

/**
 * Does all the appropriate accounting for bio completions
 *
 * @param bio  the bio to count
 **/
void countCompletedBios(BIO *bio);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Completes a bio relating to a kvio, causing the completion callback
 * to be invoked.
 *
 * This is used as the bi_end_io function for most of the bios created
 * within VDO and submitted to the storage device. Exceptions are the
 * flush code and the read-block code, both of which need to regain
 * control in the kernel layer after the I/O is completed.
 *
 * @param bio   The bio to complete
 **/
void completeAsyncBio(BIO *bio);
#else
/**
 * Completes a bio relating to a kvio, causing the completion callback
 * to be invoked.
 *
 * This is used as the bi_end_io function for most of the bios created
 * within VDO and submitted to the storage device. Exceptions are the
 * flush code and the read-block code, both of which need to regain
 * control in the kernel layer after the I/O is completed.
 *
 * @param bio   The bio to complete
 * @param error Possible error from underlying block device
 **/
void completeAsyncBio(BIO *bio, int error);
#endif

/**
 * Create a IOSubmitter structure for a new physical layer.
 *
 * @param [in]  threadNamePrefix  The per-device prefix to use in process names
 * @param [in]  threadCount       Number of bio-submission threads to set up
 * @param [in]  rotationInterval  Interval to use when rotating between
 *                                bio-submission threads when enqueuing work
 *                                items
 * @param [in]  maxRequestsActive Number of bios for merge tracking
 * @param [in]  layer             The kernel layer
 * @param [out] ioSubmitter       Pointer to the new data structure
 *
 * @return VDO_SUCCESS or an error
 **/
int makeIOSubmitter(const char    *threadNamePrefix,
                    unsigned int   threadCount,
                    unsigned int   rotationInterval,
                    unsigned int   maxRequestsActive,
                    KernelLayer   *layer,
                    IOSubmitter  **ioSubmitter);

/**
 * Tear down the IOSubmitter fields as needed for a physical layer.
 *
 * @param [in]  ioSubmitter    The I/O submitter data to tear down
 **/
void cleanupIOSubmitter(IOSubmitter *ioSubmitter);

/**
 * Free the IOSubmitter fields and structure as needed for a
 * physical layer. This must be called after
 * cleanupIOSubmitter(). It is used to release resources late in
 * the shutdown process to avoid or reduce the chance of race
 * conditions.
 *
 * @param [in]  ioSubmitter    The I/O submitter data to destroy
 **/
void freeIOSubmitter(IOSubmitter *ioSubmitter);

/**
 * Retrieve the aggregated read cache statistics for each bio submission
 * work queue.
 *
 * @param [in]  ioSubmitter        The I/O submitter data
 * @param [out] totalledStats      Where to store the statistics
 **/
void getBioWorkQueueReadCacheStats(IOSubmitter    *ioSubmitter,
                                   ReadCacheStats *totalledStats);

/**
 * Dump info to the kernel log about the work queue used by the
 * physical layer. For debugging only.
 *
 * @param [in]  ioSubmitter        The I/O submitter data
 **/
void dumpBioWorkQueue(IOSubmitter *ioSubmitter);


/**
 * Enqueue a work item to run in the work queue(s) used for bio
 * submissions from the physical layer.
 *
 * Outside of IOSubmitter, used only for finishing processing of empty
 * flush bios by sending them to the storage device.
 *
 * @param ioSubmitter        The I/O submitter data to update
 * @param workItem           The new work item to run
 **/
void enqueueBioWorkItem(IOSubmitter *ioSubmitter, KvdoWorkItem *workItem);

/**
 * Get the read cache used by the I/O submitter
 *
 * @param ioSubmitter       The I/O submitter data
 *
 * @return read cache
 **/
ReadCache *getIOSubmitterReadCache(IOSubmitter *ioSubmitter);

/**
 * Submit bio but don't block.
 *
 * Submits the bio to a helper work queue which sits in a loop
 * submitting bios. The worker thread may block if the target device
 * is busy, which is why we don't want to do the submission in the
 * original calling thread.
 *
 * The bi_private field of the bio must point to a KVIO associated
 * with the operation. The bi_end_io callback is invoked when the I/O
 * operation completes.
 *
 * @param bio      the block I/O operation descriptor to submit
 * @param action   the action code specifying the priority for the operation
 **/
void submitBio(BIO *bio, BioQAction action);

#endif // IOSUBMITTER_H
