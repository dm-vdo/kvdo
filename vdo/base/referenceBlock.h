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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/referenceBlock.h#1 $
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
  EMPTY_REFERENCE_COUNT       = 0,
  MAXIMUM_REFERENCE_COUNT     = 254,
  PROVISIONAL_REFERENCE_COUNT = 255,
};

enum {
  COUNTS_PER_SECTOR = ((VDO_SECTOR_SIZE - sizeof(PackedJournalPoint))
                       / sizeof(ReferenceCount)),
  COUNTS_PER_BLOCK  = COUNTS_PER_SECTOR * SECTORS_PER_BLOCK,
};

/**
 * The format of a ReferenceSector on disk.
 **/
typedef struct {
  PackedJournalPoint commitPoint;
  ReferenceCount     counts[COUNTS_PER_SECTOR];
} __attribute__((packed)) PackedReferenceSector;

typedef struct {
  PackedReferenceSector sectors[SECTORS_PER_BLOCK];
} PackedReferenceBlock;

/*
 * ReferenceBlock structure
 *
 * Blocks are used as a proxy, permitting saves of partial refcounts.
 **/
typedef struct {
  /** This block waits on the refCounts to tell it to write */
  Waiter          waiter;
  /** The parent RefCount structure */
  RefCounts      *refCounts;
  /** The number of references in this block that represent allocations */
  BlockSize       allocatedCount;
  /** The slab journal block on which this block must hold a lock */
  SequenceNumber  slabJournalLock;
  /**
   * The slab journal block which should be released when this block
   * is committed
   **/
  SequenceNumber  slabJournalLockToRelease;
  /** The point up to which each sector is accurate on disk */
  JournalPoint    commitPoints[SECTORS_PER_BLOCK];
  /** Whether this block has been modified since it was written to disk */
  bool            isDirty;
  /** Whether this block is currently writing */
  bool            isWriting;
} ReferenceBlock;

#endif // REFERENCE_BLOCK_H
