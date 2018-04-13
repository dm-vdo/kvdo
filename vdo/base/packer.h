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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/packer.h#1 $
 */

#ifndef PACKER_H
#define PACKER_H

#include "completion.h"
#include "physicalLayer.h"
#include "statistics.h"
#include "threadConfig.h"
#include "types.h"

enum {
  DEFAULT_PACKER_INPUT_BINS  = 16,
  DEFAULT_PACKER_OUTPUT_BINS = 256,
};

typedef struct packer Packer;

/**
 * Make a new block packer.
 *
 * @param [in]  layer           The physical layer to which compressed blocks
 *                              will be written
 * @param [in]  inputBinCount   The number of partial bins to keep in memory
 * @param [in]  outputBinCount  The number of compressed blocks that can be
 *                              written concurrently
 * @param [in]  threadConfig    The thread configuration of the VDO
 * @param [out] packerPtr       A pointer to hold the new packer
 *
 * @return VDO_SUCCESS or an error
 **/
int makePacker(PhysicalLayer       *layer,
               BlockCount           inputBinCount,
               BlockCount           outputBinCount,
               const ThreadConfig  *threadConfig,
               Packer             **packerPtr)
  __attribute__((warn_unused_result));

/**
 * Free a block packer and null out the reference to it.
 *
 * @param packerPtr  A pointer to the packer to free
 **/
void freePacker(Packer **packerPtr);

/**
 * Check whether the compressed data in a DataVIO will fit in a packer bin.
 *
 * @param dataVIO  The DataVIO
 *
 * @return <code>true</code> if the DataVIO will fit in a bin
 **/
bool isSufficientlyCompressible(DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Get the thread ID of the packer's zone.
 *
 * @param packer  The packer
 *
 * @return The packer's thread ID
 **/
ThreadID getPackerThreadID(Packer *packer);

/**
 * Get the current statistics from the packer.
 *
 * @param packer  The packer to query
 *
 * @return a copy of the current statistics for the packer
 **/
PackerStatistics getPackerStatistics(const Packer *packer)
  __attribute__((warn_unused_result));

/**
 * Attempt to rewrite the data in this DataVIO as part of a compressed block.
 *
 * @param dataVIO  The DataVIO to pack
 **/
void attemptPacking(DataVIO *dataVIO);

/**
 * Request that the packer flush asynchronously. All bins with at least two
 * compressed data blocks will be written out, and any solitary pending VIOs
 * will be released from the packer. While flushing is in progress, any VIOs
 * submitted to attemptPacking() will be continued immediately without
 * attempting to pack them.
 *
 * @param packer  The packer to flush
 **/
void flushPacker(Packer *packer);

/**
 * Remove a lock holder from the packer.
 *
 * @param completion  The DataVIO which needs a lock held by a DataVIO in the
 *                    packer. The dataVIO's compressedVIO.lockHolder field will
 *                    point to the DataVIO to remove.
 **/
void removeLockHolderFromPacker(VDOCompletion *completion);

/**
 * Increment the flush generation in the packer. This will also cause the
 * packer to flush so that any VIOs from previous generations will exit the
 * packer.
 *
 * @param packer  The packer
 **/
void incrementPackerFlushGeneration(Packer *packer);

/**
 * Close the packer. Prevent any more VIOs from entering the packer and then
 * flush.
 *
 * @param packer            The packer to flush
 * @param completion        The completion to finish when the packer is closed
 **/
void closePacker(Packer *packer, VDOCompletion *completion);

/**
 * Dump the packer, in a thread-unsafe fashion.
 *
 * @param packer  The packer
 **/
void dumpPacker(const Packer *packer);

#endif /* PACKER_H */
