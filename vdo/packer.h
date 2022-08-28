/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef PACKER_H
#define PACKER_H

#include <linux/list.h>

#include "compiler.h"

#include "admin-state.h"
#include "block-mapping-state.h"
#include "statistics.h"
#include "types.h"
#include "wait-queue.h"

enum {
	DEFAULT_PACKER_BINS = 16,
};

/*
 * Each packer_bin holds an incomplete batch of data_vios that only partially
 * fill a compressed block. The bins are kept in a ring sorted by the amount of
 * unused space so the first bin with enough space to hold a newly-compressed
 * data_vio can easily be found. When the bin fills up or is flushed, the first
 * uncanceled data_vio in the bin is selected to be the agent for that
 * bin. Upon entering the packer, each data_vio already has its compressed data
 * in the first slot of the data_vio's compressed_block (overlaid on the
 * data_vio's scratch_block). So the agent's fragment is already in place.  The
 * fragments for the other uncanceled data_vios in the bin are packed into the
 * agent's compressed block. The agent then writes out the compressed block.
 * If the write is successful, the agent shares its pbn lock which each of the
 * other data_vios in its compressed block and sends each on its way. Finally
 * the agent itself continues on the write path as before.
 *
 * There is one special bin which is used to hold data_vios which have been
 * canceled and removed from their bin by the packer. These data_vios need to
 * wait for the canceller to rendezvous with them (VDO-2809) and so they sit in
 * this special bin.
 */
struct packer_bin {
	/* List links for packer.packer_bins */
	struct list_head list;
	/* The number of items in the bin */
	slot_number_t slots_used;
	/*
	 * The number of compressed block bytes remaining in the current batch
	 */
	size_t free_space;
	/* The current partial batch of data_vios, waiting for more */
	struct data_vio *incoming[];
};

struct packer {
	/* The ID of the packer's callback thread */
	thread_id_t thread_id;
	/* The number of bins */
	block_count_t size;
	/* The block size minus header size */
	size_t bin_data_size;
	/* The number of compression slots */
	size_t max_slots;
	/* A list of all packer_bins, kept sorted by free_space */
	struct list_head bins;
	/*
	 * A bin to hold data_vios which were canceled out of the packer and
	 * are waiting to rendezvous with the canceling data_vio.
	 */
	struct packer_bin *canceled_bin;

	/* The current flush generation */
	sequence_number_t flush_generation;

	/* The administrative state of the packer */
	struct admin_state state;

	/*
	 * Statistics are only updated on the packer thread, but are
	 * accessed from other threads.
	 */
	struct packer_statistics statistics;
};

int __must_check vdo_make_packer(struct vdo *vdo,
				 block_count_t bin_count,
				 struct packer **packer_ptr);

void vdo_free_packer(struct packer *packer);

bool __must_check
vdo_data_is_sufficiently_compressible(struct data_vio *data_vio);

struct packer_statistics __must_check
vdo_get_packer_statistics(const struct packer *packer);

void vdo_attempt_packing(struct data_vio *data_vio);

void vdo_flush_packer(struct packer *packer);

void vdo_remove_lock_holder_from_packer(struct vdo_completion *completion);

void vdo_increment_packer_flush_generation(struct packer *packer);

void
vdo_drain_packer(struct packer *packer, struct vdo_completion *completion);

void vdo_resume_packer(struct packer *packer, struct vdo_completion *parent);

void vdo_dump_packer(const struct packer *packer);

#endif /* PACKER_H */
