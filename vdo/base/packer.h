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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/packer.h#12 $
 */

#ifndef PACKER_H
#define PACKER_H

#include "completion.h"
#include "physicalLayer.h"
#include "statistics.h"
#include "threadConfig.h"
#include "types.h"

enum {
	DEFAULT_PACKER_INPUT_BINS = 16,
	DEFAULT_PACKER_OUTPUT_BINS = 256,
};

struct packer;

/**
 * Make a new block packer.
 *
 * @param [in]  layer             The physical layer to which compressed blocks
 *                                will be written
 * @param [in]  input_bin_count   The number of partial bins to keep in memory
 * @param [in]  output_bin_count  The number of compressed blocks that can be
 *                                written concurrently
 * @param [in]  thread_config     The thread configuration of the VDO
 * @param [out] packer_ptr        A pointer to hold the new packer
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check make_packer(PhysicalLayer *layer,
			     block_count_t input_bin_count,
			     block_count_t output_bin_count,
			     const struct thread_config *thread_config,
			     struct packer **packer_ptr);

/**
 * Free a block packer and null out the reference to it.
 *
 * @param packer_ptr  A pointer to the packer to free
 **/
void free_packer(struct packer **packer_ptr);

/**
 * Check whether the compressed data in a data_vio will fit in a packer bin.
 *
 * @param data_vio  The data_vio
 *
 * @return <code>true</code> if the data_vio will fit in a bin
 **/
bool __must_check is_sufficiently_compressible(struct data_vio *data_vio);

/**
 * Get the thread ID of the packer's zone.
 *
 * @param packer  The packer
 *
 * @return The packer's thread ID
 **/
thread_id_t get_packer_thread_id(struct packer *packer);

/**
 * Get the current statistics from the packer.
 *
 * @param packer  The packer to query
 *
 * @return a copy of the current statistics for the packer
 **/
struct packer_statistics __must_check
get_packer_statistics(const struct packer *packer);

/**
 * Attempt to rewrite the data in this data_vio as part of a compressed block.
 *
 * @param data_vio  The data_vio to pack
 **/
void attempt_packing(struct data_vio *data_vio);

/**
 * Request that the packer flush asynchronously. All bins with at least two
 * compressed data blocks will be written out, and any solitary pending VIOs
 * will be released from the packer. While flushing is in progress, any VIOs
 * submitted to attempt_packing() will be continued immediately without
 * attempting to pack them.
 *
 * @param packer  The packer to flush
 **/
void flush_packer(struct packer *packer);

/**
 * Remove a lock holder from the packer.
 *
 * @param completion  The data_vio which needs a lock held by a data_vio in the
 *                    packer. The data_vio's compressedVIO.lockHolder field will
 *                    point to the data_vio to remove.
 **/
void remove_lock_holder_from_packer(struct vdo_completion *completion);

/**
 * Increment the flush generation in the packer. This will also cause the
 * packer to flush so that any VIOs from previous generations will exit the
 * packer.
 *
 * @param packer  The packer
 **/
void increment_packer_flush_generation(struct packer *packer);

/**
 * Drain the packer by preventing any more VIOs from entering the packer and
 * then flushing.
 *
 * @param packer      The packer to drain
 * @param completion  The completion to finish when the packer has drained
 **/
void drain_packer(struct packer *packer, struct vdo_completion *completion);

/**
 * Resume a packer which has been suspended.
 *
 * @param packer  The packer to resume
 * @param parent  The completion to finish when the packer has resumed
 *
 * @return VDO_SUCCESS or an error
 **/
void resume_packer(struct packer *packer, struct vdo_completion *parent);

/**
 * Dump the packer, in a thread-unsafe fashion.
 *
 * @param packer  The packer
 **/
void dump_packer(const struct packer *packer);

#endif /* PACKER_H */
