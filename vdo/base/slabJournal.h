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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabJournal.h#21 $
 */

#ifndef SLAB_JOURNAL_H
#define SLAB_JOURNAL_H

#include "completion.h"
#include "journalPoint.h"
#include "list.h"
#include "types.h"

/**
 * Convert a completion to a slab_journal.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a slab_journal
 **/
struct slab_journal * __must_check
as_slab_journal(struct vdo_completion *completion);

/**
 * Calculate the number of slab journal entries per block.
 *
 * @return The number of slab journal entries per block
 **/
size_t __must_check get_slab_journal_entries_per_block(void);

/**
 * Obtain a pointer to a slab_journal structure from a pointer to the
 * dirty list entry field within it.
 *
 * @param entry  The list entry to convert
 *
 * @return The entry as a slab_journal
 **/
struct slab_journal * __must_check
slab_journal_from_dirty_entry(struct list_head *entry);

/**
 * Create a slab journal.
 *
 * @param [in]  allocator         The block allocator which owns this journal
 * @param [in]  slab              The parent slab of the journal
 * @param [in]  recovery_journal  The recovery journal of the VDO
 * @param [out] journal_ptr       The pointer to hold the new slab journal
 *
 * @return VDO_SUCCESS or error code
 **/
int __must_check make_slab_journal(struct block_allocator *allocator,
				   struct vdo_slab *slab,
				   struct recovery_journal *recovery_journal,
				   struct slab_journal **journal_ptr);

/**
 * Free a slab journal and null out the reference to it.
 *
 * @param journal_ptr  The reference to the slab journal to free
 **/
void free_slab_journal(struct slab_journal **journal_ptr);

/**
 * Check whether a slab journal is blank, meaning it has never had any entries
 * recorded in it.
 *
 * @param journal  The journal to query
 *
 * @return <code>true</code> if the slab journal has never been modified
 **/
bool __must_check is_slab_journal_blank(const struct slab_journal *journal);

/**
 * Check whether the slab journal is on the block allocator's list of dirty
 * journals.
 *
 * @param journal  The journal to query
 *
 * @return <code>true</code> if the journal has been added to the dirty list
 **/
bool __must_check is_slab_journal_dirty(const struct slab_journal *journal);

/**
 * Check whether a slab journal is active.
 *
 * @param journal  The slab journal to check
 *
 * @return <code>true</code> if the journal is active
 **/
bool __must_check is_slab_journal_active(struct slab_journal *journal);

/**
 * Abort any VIOs waiting to make slab journal entries.
 *
 * @param journal  The journal to abort
 **/
void abort_slab_journal_waiters(struct slab_journal *journal);

/**
 * Reopen a slab journal by emptying it and then adding any pending entries.
 *
 * @param journal  The journal to reopen
 **/
void reopen_slab_journal(struct slab_journal *journal);

/**
 * Attempt to replay a recovery journal entry into a slab journal.
 *
 * @param journal         The slab journal to use
 * @param pbn             The PBN for the entry
 * @param operation       The type of entry to add
 * @param recovery_point  The recovery journal point corresponding to this entry
 * @param parent          The completion to notify when there is space to add
 *                        the entry if the entry could not be added immediately
 *
 * @return <code>true</code> if the entry was added immediately
 **/
bool __must_check
attempt_replay_into_slab_journal(struct slab_journal *journal,
				 physical_block_number_t pbn,
				 journal_operation operation,
				 struct journal_point *recovery_point,
				 struct vdo_completion *parent);

/**
 * Add an entry to a slab journal.
 *
 * @param journal   The slab journal to use
 * @param data_vio  The data_vio for which to add the entry
 **/
void add_slab_journal_entry(struct slab_journal *journal,
			    struct data_vio *data_vio);

/**
 * Adjust the reference count for a slab journal block. Note that when the
 * adjustment is negative, the slab journal will be reaped.
 *
 * @param journal          The slab journal
 * @param sequence_number  The journal sequence number of the referenced block
 * @param adjustment       Amount to adjust the reference counter
 **/
void adjust_slab_journal_block_reference(struct slab_journal *journal,
					 sequence_number_t sequence_number,
					 int adjustment);

/**
 * Request the slab journal to release the recovery journal lock it may hold on
 * a specified recovery journal block.
 *
 * @param journal        The slab journal
 * @param recovery_lock  The sequence number of the recovery journal block
 *                       whose locks should be released
 *
 * @return <code>true</code> if the journal does hold a lock on the specified
 *         block (which it will release)
 **/
bool __must_check
release_recovery_journal_lock(struct slab_journal *journal,
			      sequence_number_t recovery_lock);

/**
 * Commit the tail block of a slab journal.
 *
 * @param journal  The journal whose tail block should be committed
 **/
void commit_slab_journal_tail(struct slab_journal *journal);

/**
 * Drain slab journal I/O. Depending upon the type of drain (as recorded in
 * the journal's slab), any dirty journal blocks may be written out.
 *
 * @param journal  The journal to drain
 **/
void drain_slab_journal(struct slab_journal *journal);

/**
 * Decode the slab journal by reading its tail.
 *
 * @param journal  The journal to decode
 **/
void decode_slab_journal(struct slab_journal *journal);

/**
 * Check to see if the journal should be scrubbed.
 *
 * @param journal  The slab journal
 *
 * @return <code>true</code> if the journal requires scrubbing
 **/
bool __must_check requires_scrubbing(const struct slab_journal *journal);

/**
 * Dump the slab journal.
 *
 * @param journal       The slab journal to dump
 **/
void dump_slab_journal(const struct slab_journal *journal);

#endif // SLAB_JOURNAL_H
