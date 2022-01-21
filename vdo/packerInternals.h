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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/packerInternals.h#5 $
 */

#ifndef PACKER_INTERNALS_H
#define PACKER_INTERNALS_H

#include <linux/list.h>

#include "packer.h"

#include "adminState.h"
#include "compressedBlock.h"
#include "header.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

/**
 * Each input_bin holds an incomplete batch of data_vios that only partially
 * fill a compressed block. The input bins are kept in a ring sorted by the
 * amount of unused space so the first bin with enough space to hold a
 * newly-compressed data_vio can easily be found. When the bin fills up or is
 * flushed, the incoming data_vios are moved to the packer's batched_data_vios
 * queue, from which they will eventually be routed to an idle output_bin.
 *
 * There is one special input bin which is used to hold data_vios which have
 * been canceled and removed from their input bin by the packer. These
 * data_vios need to wait for the canceller to rendezvous with them (VDO-2809)
 * and so they sit in this special bin.
 **/
struct input_bin {
	/** List links for packer.input_bins */
	struct list_head list;
	/** The number of items in the bin */
	slot_number_t slots_used;
	/**
	 * The number of compressed block bytes remaining in the current batch
	 */
	size_t free_space;
	/** The current partial batch of data_vios, waiting for more */
	struct data_vio *incoming[];
};

/**
 * Each output_bin allows a single compressed block to be packed and written.
 * When it is not idle, it holds a batch of data_vios that have been packed
 * into the compressed block, written asynchronously, and are waiting for the
 * write to complete.
 **/
struct output_bin {
	/** List links for packer.output_bins */
	struct list_head list;
	/** The storage for encoding the compressed block representation */
	struct compressed_block *block;
	/**
	 * The struct allocating_vio wrapping the compressed block for writing
	 */
	struct allocating_vio *writer;
	/** The number of compression slots used in the compressed block */
	slot_number_t slots_used;
	/** The data_vios packed into the block, waiting for the write to
	 * complete */
	struct wait_queue outgoing;
};

/**
 * A counted array holding a batch of data_vios that should be packed into an
 * output bin.
 **/
struct output_batch {
	size_t slots_used;
	struct data_vio *slots[VDO_MAX_COMPRESSION_SLOTS];
};

struct packer {
	/** The ID of the packer's callback thread */
	thread_id_t thread_id;
	/** The selector determining which physical zone to allocate from */
	struct allocation_selector *selector;
	/** The number of input bins */
	block_count_t size;
	/** The block size minus header size */
	size_t bin_data_size;
	/** The number of compression slots */
	size_t max_slots;
	/** A list of all input_bins, kept sorted by free_space */
	struct list_head input_bins;
	/** A list of all output_bins */
	struct list_head output_bins;
	/**
	 * A bin to hold data_vios which were canceled out of the packer and
	 * are waiting to rendezvous with the canceling data_vio.
	 **/
	struct input_bin *canceled_bin;

	/** The current flush generation */
	sequence_number_t flush_generation;

	/** The administrative state of the packer */
	struct admin_state state;
	/** True when writing batched data_vios */
	bool writing_batches;

	/**
	 * Statistics are only updated on the packer thread, but are
	 * accessed from other threads.
	 **/
	struct packer_statistics statistics;

	/** Queue of batched data_vios waiting to be packed */
	struct wait_queue batched_data_vios;

	/** The total number of output bins allocated */
	size_t output_bin_count;
	/** The number of idle output bins on the stack */
	size_t idle_output_bin_count;
	/** The stack of idle output bins (0 = bottom) */
	struct output_bin *idle_output_bins[];
};


#endif /* PACKER_INTERNALS_H */
