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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recovery-journal.h#1 $
 */

#ifndef RECOVERY_JOURNAL_H
#define RECOVERY_JOURNAL_H

#include <linux/list.h>

#include "numeric.h"

#include "admin-state.h"
#include "completion.h"
#include "fixed-layout.h"
#include "flush.h"
#include "journal-point.h"
#include "lock-counter.h"
#include "read-only-notifier.h"
#include "recovery-journal.h"
#include "recovery-journal-format.h"
#include "statistics.h"
#include "types.h"
#include "wait-queue.h"

/**
 * The recovery_journal provides a log of all block mapping and reference count
 * changes which have not yet been stably written to the block map or slab
 * journals. This log helps to reduce the write amplification of writes by
 * providing amortization of slab journal and block map page updates.
 *
 * The journal consists of a set of on-disk blocks arranged as a
 * circular log with monotonically increasing sequence numbers. Three
 * sequence numbers serve to define the active extent of the
 * journal. The 'head' is the oldest active block in the journal. The
 * 'tail' is the end of the half-open interval containing the active
 * blocks. 'active' is the number of the block actively receiving
 * entries. In an empty journal, head == active == tail. Once any
 * entries are added, tail = active + 1, and head may be any value in
 * the interval [tail - size, active].
 *
 * The journal also contains a set of in-memory blocks which are used
 * to buffer up entries until they can be committed. In general the
 * number of in-memory blocks ('tail_buffer_count') will be less than
 * the on-disk size. Each in-memory block is also a vdo_completion.
 * Each in-memory block has a vdo_extent which is used to commit that
 * block to disk. The extent's data is the on-disk representation
 * of the journal block. In addition each in-memory block has a
 * buffer which is used to accumulate entries while a partial commit
 * of the block is in progress. In-memory blocks are kept on two
 * rings. Free blocks live on the 'free_tail_blocks' ring. When a block
 * becomes active (see below) it is moved to the 'active_tail_blocks'
 * ring. When a block is fully committed, it is moved back to the
 * 'free_tail_blocks' ring.
 *
 * When entries are added to the journal, they are added to the active
 * in-memory block, as indicated by the 'active_block' field. If the
 * caller wishes to wait for the entry to be committed, the requesting
 * VIO will be attached to the in-memory block to which the caller's
 * entry was added. If the caller does wish to wait, or if the entry
 * filled the active block, an attempt will be made to commit that
 * block to disk. If there is already another commit in progress, the
 * attempt will be ignored and then automatically retried when the
 * in-progress commit completes. If there is no commit in progress,
 * any VIOs waiting on the block are transferred to the extent. The
 * extent is then written, automatically waking all of the waiters
 * when it completes. When the extent completes, any entries which
 * accumulated in the block are copied to the extent's data buffer.
 *
 * Finally, the journal maintains a set of counters, one for each on
 * disk journal block. These counters are used as locks to prevent
 * premature reaping of journal blocks. Each time a new sequence
 * number is used, the counter for the corresponding block is
 * incremented. The counter is subsequently decremented when that
 * block is filled and then committed for the last time. This prevents
 * blocks from being reaped while they are still being updated. The
 * counter is also incremented once for each entry added to a block,
 * and decremented once each time the block map is updated in memory
 * for that request. This prevents blocks from being reaped while
 * their VIOs are still active. Finally, each in-memory block map page
 * tracks the oldest journal block that contains entries corresponding to
 * uncommitted updates to that block map page. Each time an in-memory block
 * map page is updated, it checks if the journal block for the VIO
 * is earlier than the one it references, in which case it increments
 * the count on the earlier journal block and decrements the count on the
 * later journal block, maintaining a lock on the oldest journal block
 * containing entries for that page. When a block map page has been flushed
 * from the cache, the counter for the journal block it references is
 * decremented. Whenever the counter for the head block goes to 0, the
 * head is advanced until it comes to a block whose counter is not 0
 * or until it reaches the active block. This is the mechanism for
 * reclaiming journal space on disk.
 *
 * If there is no in-memory space when a VIO attempts to add an entry,
 * the VIO will be attached to the 'commit_completion' and will be
 * woken the next time a full block has committed. If there is no
 * on-disk space when a VIO attempts to add an entry, the VIO will be
 * attached to the 'reap_completion', and will be woken the next time a
 * journal block is reaped.
 **/

 struct recovery_journal {
	/** The thread ID of the journal zone */
	thread_id_t thread_id;
	/** The slab depot which can hold locks on this journal */
	struct slab_depot *depot;
	/** The block map which can hold locks on this journal */
	struct block_map *block_map;
	/** The queue of vios waiting to make increment entries */
	struct wait_queue increment_waiters;
	/** The queue of vios waiting to make decrement entries */
	struct wait_queue decrement_waiters;
	/** The number of free entries in the journal */
	uint64_t available_space;
	/** The number of decrement entries which need to be made */
	vio_count_t pending_decrement_count;
	/**
	 * Whether the journal is adding entries from the increment or
	 * decrement waiters queues
	 **/
	bool adding_entries;
	/** The notifier for read-only mode */
	struct read_only_notifier *read_only_notifier;
	/** The administrative state of the journal */
	struct admin_state state;
	/** Whether a reap is in progress */
	bool reaping;
	/** The partition which holds the journal on disk */
	struct partition *partition;
	/** The oldest active block in the journal on disk for block map rebuild
	 */
	sequence_number_t block_map_head;
	/** The oldest active block in the journal on disk for slab journal
	 * replay */
	sequence_number_t slab_journal_head;
	/** The newest block in the journal on disk to which a write has
	 * finished */
	sequence_number_t last_write_acknowledged;
	/** The end of the half-open interval of the active journal */
	sequence_number_t tail;
	/** The point at which the last entry will have been added */
	struct journal_point append_point;
	/** The journal point of the vio most recently released from the journal
	 */
	struct journal_point commit_point;
	/** The nonce of the VDO */
	nonce_t nonce;
	/** The number of recoveries completed by the VDO */
	uint8_t recovery_count;
	/** The number of entries which fit in a single block */
	journal_entry_count_t entries_per_block;
	/** Unused in-memory journal blocks */
	struct list_head free_tail_blocks;
	/** In-memory journal blocks with records */
	struct list_head active_tail_blocks;
	/** A pointer to the active block (the one we are adding entries to now)
	 */
	struct recovery_journal_block *active_block;
	/** Journal blocks that need writing */
	struct wait_queue pending_writes;
	/** The new block map reap head after reaping */
	sequence_number_t block_map_reap_head;
	/** The head block number for the block map rebuild range */
	block_count_t block_map_head_block_number;
	/** The new slab journal reap head after reaping */
	sequence_number_t slab_journal_reap_head;
	/** The head block number for the slab journal replay range */
	block_count_t slab_journal_head_block_number;
	/** The data-less vio, usable only for flushing */
	struct vio *flush_vio;
	/** The number of blocks in the on-disk journal */
	block_count_t size;
	/** The number of logical blocks that are in-use */
	block_count_t logical_blocks_used;
	/** The number of block map pages that are allocated */
	block_count_t block_map_data_blocks;
	/** The number of journal blocks written but not yet acknowledged */
	block_count_t pending_write_count;
	/** The threshold at which slab journal tail blocks will be written out
	 */
	block_count_t slab_journal_commit_threshold;
	/** Counters for events in the journal that are reported as statistics
	 */
	struct recovery_journal_statistics events;
	/** The locks for each on-disk block */
	struct lock_counter *lock_counter;
};

/**
 * Get the physical block number for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number of the desired block
 *
 * @return The block number corresponding to the sequence number
 **/
static inline physical_block_number_t __must_check
get_vdo_recovery_journal_block_number(const struct recovery_journal *journal,
				      sequence_number_t sequence)
{
	// Since journal size is a power of two, the block number modulus can
	// just be extracted from the low-order bits of the sequence.
	return compute_vdo_recovery_journal_block_number(journal->size, sequence);
}

/**
 * Compute the check byte for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number
 *
 * @return The check byte corresponding to the sequence number
 **/
static inline uint8_t __must_check
compute_vdo_recovery_journal_check_byte(const struct recovery_journal *journal,
					sequence_number_t sequence)
{
	// The check byte must change with each trip around the journal.
	return (((sequence / journal->size) & 0x7F) | 0x80);
}

/**
 * Return whether a given journal_operation is an increment type.
 *
 * @param operation  The operation in question
 *
 * @return true if the type is an increment type
 **/
static inline bool
is_vdo_journal_increment_operation(enum journal_operation operation)
{
	return ((operation == VDO_JOURNAL_DATA_INCREMENT)
		|| (operation == VDO_JOURNAL_BLOCK_MAP_INCREMENT));
}

/**
 * Make a recovery journal and initialize it with the state that was decoded
 * from the super block.
 *
 * @param [in]  state               the decoded state of the journal
 * @param [in]  nonce               the nonce of the VDO
 * @param [in]  vdo                 the VDO
 * @param [in]  partition           the partition for the journal
 * @param [in]  recovery_count      the VDO's number of completed recoveries
 * @param [in]  journal_size        the number of blocks in the journal on disk
 * @param [in]  tail_buffer_size    the number of blocks for tail buffer
 * @param [in]  read_only_notifier  the read-only mode notifier
 * @param [in]  thread_config       the thread configuration of the VDO
 * @param [out] journal_ptr         the pointer to hold the new recovery journal
 *
 * @return a success or error code
 **/
int __must_check
decode_vdo_recovery_journal(struct recovery_journal_state_7_0 state,
			    nonce_t nonce,
			    struct vdo *vdo,
			    struct partition *partition,
			    uint64_t recovery_count,
			    block_count_t journal_size,
			    block_count_t tail_buffer_size,
			    struct read_only_notifier *read_only_notifier,
			    const struct thread_config *thread_config,
			    struct recovery_journal **journal_ptr);

/**
 * Free a recovery journal.
 *
 * @param journal  The recovery journal to free
 **/
void free_vdo_recovery_journal(struct recovery_journal *journal);

/**
 * Move the backing partition pointer of the recovery journal.
 * Assumes that the data in the old and the new partitions is identical.
 *
 * @param journal   the journal being moved
 * @param partition the new journal partition
 **/
void set_vdo_recovery_journal_partition(struct recovery_journal *journal,
					struct partition *partition);

/**
 * Initialize the journal after a recovery.
 *
 * @param journal         The journal in question
 * @param recovery_count  The number of completed recoveries
 * @param tail            The new tail block sequence number
 **/
void
initialize_vdo_recovery_journal_post_recovery(struct recovery_journal *journal,
					      uint64_t recovery_count,
					      sequence_number_t tail);

/**
 * Initialize the journal after a rebuild.
 *
 * @param journal               The journal in question
 * @param recovery_count        The number of completed recoveries
 * @param tail                  The new tail block sequence number
 * @param logical_blocks_used   The new number of logical blocks used
 * @param block_map_data_blocks The new number of block map data blocks
 **/
void
initialize_vdo_recovery_journal_post_rebuild(struct recovery_journal *journal,
					     uint64_t recovery_count,
					     sequence_number_t tail,
					     block_count_t logical_blocks_used,
					     block_count_t block_map_data_blocks);

/**
 * Get the number of block map pages, allocated from data blocks, currently
 * in use.
 *
 * @param journal   The journal in question
 *
 * @return  The number of block map pages allocated from slabs
 **/
block_count_t __must_check
vdo_get_journal_block_map_data_blocks_used(struct recovery_journal *journal);

/**
 * Get the ID of a recovery journal's thread.
 *
 * @param journal  The journal to query
 *
 * @return The ID of the journal's thread.
 **/
thread_id_t __must_check
get_vdo_recovery_journal_thread_id(struct recovery_journal *journal);

/**
 * Prepare the journal for new entries.
 *
 * @param journal    The journal in question
 * @param depot      The slab depot for this VDO
 * @param block_map  The block map for this VDO
 **/
void open_vdo_recovery_journal(struct recovery_journal *journal,
			       struct slab_depot *depot,
			       struct block_map *block_map);

/**
 * Obtain the recovery journal's current sequence number. Exposed only so
 * the block map can be initialized therefrom.
 *
 * @param journal  The journal in question
 *
 * @return the sequence number of the tail block
 **/
sequence_number_t
get_vdo_recovery_journal_current_sequence_number(struct recovery_journal *journal);

/**
 * Get the number of usable recovery journal blocks.
 *
 * @param journal_size  The size of the recovery journal in blocks
 *
 * @return the number of recovery journal blocks usable for entries
 **/
block_count_t __must_check
get_vdo_recovery_journal_length(block_count_t journal_size);

/**
 * Record the state of a recovery journal for encoding in the super block.
 *
 * @param journal  the recovery journal
 *
 * @return the state of the journal
 **/
struct recovery_journal_state_7_0 __must_check
record_vdo_recovery_journal(const struct recovery_journal *journal);

/**
 * Add an entry to a recovery journal. This method is asynchronous. The
 * data_vio will not be called back until the entry is committed to the
 * on-disk journal.
 *
 * @param journal   The journal in which to make an entry
 * @param data_vio  The data_vio for which to add the entry. The entry will be
 *                  taken from the logical and new_mapped fields of the
 *                  data_vio. The data_vio's recovery_sequence_number field
 *                  will be set to the sequence number of the journal block in
 *                  which the entry was made.
 **/
void add_vdo_recovery_journal_entry(struct recovery_journal *journal,
				    struct data_vio *data_vio);

/**
 * Acquire a reference to a recovery journal block from somewhere other than
 * the journal itself.
 *
 * @param journal          The recovery journal
 * @param sequence_number  The journal sequence number of the referenced block
 * @param zone_type        The type of the zone making the adjustment
 * @param zone_id          The ID of the zone making the adjustment
 **/
void acquire_vdo_recovery_journal_block_reference(struct recovery_journal *journal,
						  sequence_number_t sequence_number,
						  enum vdo_zone_type zone_type,
						  zone_count_t zone_id);

/**
 * Release a reference to a recovery journal block from somewhere other than
 * the journal itself. If this is the last reference for a given zone type,
 * an attempt will be made to reap the journal.
 *
 * @param journal          The recovery journal
 * @param sequence_number  The journal sequence number of the referenced block
 * @param zone_type        The type of the zone making the adjustment
 * @param zone_id          The ID of the zone making the adjustment
 **/
void release_vdo_recovery_journal_block_reference(struct recovery_journal *journal,
						  sequence_number_t sequence_number,
						  enum vdo_zone_type zone_type,
						  zone_count_t zone_id);

/**
 * Release a single per-entry reference count for a recovery journal block. This
 * method may be called from any zone (but shouldn't be called from the journal
 * zone as it would be inefficient).
 *
 * @param journal          The recovery journal
 * @param sequence_number  The journal sequence number of the referenced block
 **/
void vdo_release_journal_per_entry_lock_from_other_zone(struct recovery_journal *journal,
							sequence_number_t sequence_number);

/**
 * Drain recovery journal I/O. All uncommitted entries will be written out.
 *
 * @param journal    The journal to drain
 * @param operation  The drain operation (suspend or save)
 * @param parent     The completion to finish once the journal is drained
 **/
void drain_vdo_recovery_journal(struct recovery_journal *journal,
				const struct admin_state_code *operation,
				struct vdo_completion *parent);

/**
 * Resume a recovery journal which has been drained.
 *
 * @param journal  The journal to resume
 * @param parent   The completion to finish once the journal is resumed
 **/
void resume_vdo_recovery_journal(struct recovery_journal *journal,
				 struct vdo_completion *parent);

/**
 * Get the number of logical blocks in use by the VDO
 *
 * @param journal   the journal
 *
 * @return the number of logical blocks in use by the VDO
 **/
block_count_t __must_check
get_vdo_recovery_journal_logical_blocks_used(const struct recovery_journal *journal);

/**
 * Get the current statistics from the recovery journal.
 *
 * @param journal   The recovery journal to query
 *
 * @return a copy of the current statistics for the journal
 **/
struct recovery_journal_statistics __must_check
get_vdo_recovery_journal_statistics(const struct recovery_journal *journal);

/**
 * Dump some current statistics and other debug info from the recovery
 * journal.
 *
 * @param journal   The recovery journal to dump
 **/
void dump_vdo_recovery_journal_statistics(const struct recovery_journal *journal);

#endif // RECOVERY_JOURNAL_H
