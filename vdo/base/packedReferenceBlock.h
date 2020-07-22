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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/packedReferenceBlock.h#2 $
 */

#ifndef PACKED_REFERENCE_BLOCK_H
#define PACKED_REFERENCE_BLOCK_H

#include "constants.h"
#include "journalPoint.h"
#include "types.h"

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

#endif // PACKED_REFERENCE_BLOCK_H
