// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "recovery-utils.h"

#include <linux/bio.h>

#include "logger.h"
#include "memory-alloc.h"

#include "completion.h"
#include "io-submitter.h"
#include "kernel-types.h"
#include "num-utils.h"
#include "packed-recovery-journal-block.h"
#include "recovery-journal-entry.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "types.h"
#include "vdo.h"
#include "vdo-component.h"
#include "vdo-component-states.h"
#include "vio.h"

struct journal_loader {
	struct vdo_completion *parent;
	thread_id_t thread_id;
	physical_block_number_t pbn;
	vio_count_t count;
	vio_count_t complete;
	struct vio *vios[];
};

static void free_journal_loader(struct journal_loader *loader)
{
	vio_count_t v;

	if (loader == NULL) {
		return;
	}

	for (v = 0; v < loader->count; v++) {
		free_vio(UDS_FORGET(loader->vios[v]));
	}

	UDS_FREE(loader);
}

/**
 * finish_journal_load() - Handle the completion of a journal read, and if it
 *                         is the last one, finish the load by notifying the
 *                         parent.
 **/
static void finish_journal_load(struct vdo_completion *completion)
{
	int result = completion->result;
	struct journal_loader *loader = completion->parent;

	if (++loader->complete == loader->count) {
		vdo_finish_completion(loader->parent, result);
		free_journal_loader(loader);
	}
}

static void handle_journal_load_error(struct vdo_completion *completion)
{
	record_metadata_io_error(as_vio(completion));
	completion->callback(completion);
}

static void read_journal_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct journal_loader *loader = vio->completion.parent;

	continue_vio_after_io(vio, finish_journal_load, loader->thread_id);
}

/**
 * vdo_load_recovery_journal() - Load the journal data off the disk.
 * @journal: The recovery journal to load.
 * @parent: The completion to notify when the load is complete.
 * @journal_data_ptr: A pointer to the journal data buffer (it is the
 *                    caller's responsibility to free this buffer).
 */
void vdo_load_recovery_journal(struct recovery_journal *journal,
			       struct vdo_completion *parent,
			       char **journal_data_ptr)
{
	char *ptr;
	struct journal_loader *loader;
	physical_block_number_t pbn =
		vdo_get_fixed_layout_partition_offset(journal->partition);
	vio_count_t vio_count = DIV_ROUND_UP(journal->size,
					     MAX_BLOCKS_PER_VIO);
	block_count_t remaining = journal->size;
	int result = UDS_ALLOCATE(journal->size * VDO_BLOCK_SIZE,
				  char,
				  __func__,
				  journal_data_ptr);

	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	result = UDS_ALLOCATE_EXTENDED(struct journal_loader,
				       vio_count,
				       struct vio *,
				       __func__,
				       &loader);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	loader->thread_id = vdo_get_callback_thread_id();
	loader->parent = parent;
	ptr = *journal_data_ptr;
	for (loader->count = 0; loader->count < vio_count; loader->count++) {
		unsigned short blocks =
			min(remaining, (block_count_t) MAX_BLOCKS_PER_VIO);

		result = create_multi_block_metadata_vio(parent->vdo,
							 VIO_TYPE_RECOVERY_JOURNAL,
							 VIO_PRIORITY_METADATA,
							 loader,
							 blocks,
							 ptr,
							 &loader->vios[loader->count]);
		if (result != VDO_SUCCESS) {
			free_journal_loader(UDS_FORGET(loader));
			vdo_finish_completion(parent, result);
			return;
		}

		ptr += (blocks * VDO_BLOCK_SIZE);
		remaining -= blocks;
	}

	for (vio_count = 0;
	     vio_count < loader->count;
	     vio_count++, pbn += MAX_BLOCKS_PER_VIO) {
		submit_metadata_vio(loader->vios[vio_count],
				    pbn,
				    read_journal_endio,
				    handle_journal_load_error,
				    REQ_OP_READ);
	}
}

/**
 * is_congruent_recovery_journal_block() - Determine whether the given
 *                                         header describes a valid
 *                                         block for the given journal
 *                                         that could appear at the
 *                                         given offset in the
 *                                         journal.
 * @journal: The journal to use.
 * @header: The unpacked block header to check.
 * @offset: An offset indicating where the block was in the journal.
 *
 * Return: True if the header matches.
 */
static bool __must_check
is_congruent_recovery_journal_block(struct recovery_journal *journal,
				    const struct recovery_block_header *header,
				    physical_block_number_t offset)
{
	physical_block_number_t expected_offset =
		vdo_get_recovery_journal_block_number(journal,
						      header->sequence_number);
	return ((expected_offset == offset)
		&& vdo_is_valid_recovery_journal_block(journal, header));
}

/**
 * vdo_find_recovery_journal_head_and_tail() - Find the tail and head of the
 *                                             journal.
 * @journal: The recovery journal.
 * @journal_data: The journal data read from disk.
 * @tail_ptr: A pointer to return the tail found, or if no higher
 *            block is found, the value currently in the journal.
 * @block_map_head_ptr: A pointer to return the block map head.
 * @slab_journal_head_ptr: An optional pointer to return the slab journal head.
 *
 * Finds the tail and the head of the journal by searching for the highest
 * sequence number in a block with a valid nonce, and the highest head value
 * among the blocks with valid nonces.
 *
 * Return: True if there were valid journal blocks
 */
bool vdo_find_recovery_journal_head_and_tail(struct recovery_journal *journal,
					     char *journal_data,
					     sequence_number_t *tail_ptr,
					     sequence_number_t *block_map_head_ptr,
					     sequence_number_t *slab_journal_head_ptr)
{
	sequence_number_t highest_tail = journal->tail;
	sequence_number_t block_map_head_max = 0;
	sequence_number_t slab_journal_head_max = 0;
	bool found_entries = false;
	physical_block_number_t i;

	for (i = 0; i < journal->size; i++) {
		struct packed_journal_header *packed_header =
			vdo_get_recovery_journal_block_header(journal,
							      journal_data,
							      i);
		struct recovery_block_header header;

		vdo_unpack_recovery_block_header(packed_header, &header);

		if (!is_congruent_recovery_journal_block(journal, &header, i)) {
			/*
			 * This block is old, unformatted, or doesn't belong at
			 * this location.
			 */
			continue;
		}

		if (header.sequence_number >= highest_tail) {
			found_entries = true;
			highest_tail = header.sequence_number;
		}
		if (header.block_map_head > block_map_head_max) {
			block_map_head_max = header.block_map_head;
		}
		if (header.slab_journal_head > slab_journal_head_max) {
			slab_journal_head_max = header.slab_journal_head;
		}
	}

	*tail_ptr = highest_tail;
	if (!found_entries) {
		return false;
	}

	*block_map_head_ptr = block_map_head_max;
	if (slab_journal_head_ptr != NULL) {
		*slab_journal_head_ptr = slab_journal_head_max;
	}
	return true;
}

/**
 * vdo_validate_recovery_journal_entry() - Validate a recovery journal entry.
 * @vdo: The vdo.
 * @entry: The entry to validate.
 *
 * Return: VDO_SUCCESS or an error.
 */
int
vdo_validate_recovery_journal_entry(const struct vdo *vdo,
				    const struct recovery_journal_entry *entry)
{
	if ((entry->slot.pbn >= vdo->states.vdo.config.physical_blocks) ||
	    (entry->slot.slot >= VDO_BLOCK_MAP_ENTRIES_PER_PAGE) ||
	    !vdo_is_valid_location(&entry->mapping) ||
	    !vdo_is_physical_data_block(vdo->depot, entry->mapping.pbn)) {
		return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
					      "Invalid entry: (%llu, %u) to %llu (%s) is not within bounds",
					      (unsigned long long) entry->slot.pbn,
					      entry->slot.slot,
					      (unsigned long long) entry->mapping.pbn,
					      vdo_get_journal_operation_name(entry->operation));
	}

	if ((entry->operation == VDO_JOURNAL_BLOCK_MAP_INCREMENT) &&
	    (vdo_is_state_compressed(entry->mapping.state) ||
	    (entry->mapping.pbn == VDO_ZERO_BLOCK))) {
		return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
					      "Invalid entry: (%llu, %u) to %llu (%s) is not a valid tree mapping",
					      (unsigned long long) entry->slot.pbn,
					      entry->slot.slot,
					      (unsigned long long) entry->mapping.pbn,
					      vdo_get_journal_operation_name(entry->operation));
	}

	return VDO_SUCCESS;
}
