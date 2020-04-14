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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/referenceBlock.h#7 $
 */

#ifndef REFERENCE_BLOCK_H
#define REFERENCE_BLOCK_H

#include "constants.h"
#include "journalPoint.h"
#include "types.h"
#include "waitQueue.h"

/**
 * A type representing a reference count.
 **/
typedef uint8_t ReferenceCount;

/**
 * Special ReferenceCount values.
 **/
enum {
	EMPTY_REFERENCE_COUNT = 0,
	MAXIMUM_REFERENCE_COUNT = 254,
	PROVISIONAL_REFERENCE_COUNT = 255,
};

enum {
	COUNTS_PER_SECTOR =
		((VDO_SECTOR_SIZE - sizeof(struct packed_journal_point))
		 / sizeof(ReferenceCount)),
	COUNTS_PER_BLOCK = COUNTS_PER_SECTOR * SECTORS_PER_BLOCK,
};

/**
 * The format of a ReferenceSector on disk.
 **/
struct packed_reference_sector {
	struct packed_journal_point commit_point;
	ReferenceCount counts[COUNTS_PER_SECTOR];
} __attribute__((packed));

struct packed_reference_block {
	struct packed_reference_sector sectors[SECTORS_PER_BLOCK];
};

/*
 * Reference_block structure
 *
 * Blocks are used as a proxy, permitting saves of partial refcounts.
 **/
struct reference_block {
	/** This block waits on the refCounts to tell it to write */
	struct waiter waiter;
	/** The parent RefCount structure */
	struct ref_counts *ref_counts;
	/** The number of references in this block that represent allocations */
	block_size_t allocated_count;
	/** The slab journal block on which this block must hold a lock */
	SequenceNumber slab_journal_lock;
	/**
	 * The slab journal block which should be released when this block
	 * is committed
	 **/
	SequenceNumber slab_journal_lock_to_release;
	/** The point up to which each sector is accurate on disk */
	struct journal_point commit_points[SECTORS_PER_BLOCK];
	/** Whether this block has been modified since it was written to disk */
	bool is_dirty;
	/** Whether this block is currently writing */
	bool is_writing;
};

#endif // REFERENCE_BLOCK_H
