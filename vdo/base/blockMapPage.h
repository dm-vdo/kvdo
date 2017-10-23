/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/blockMapPage.h#1 $
 */

#ifndef BLOCK_MAP_PAGE_H
#define BLOCK_MAP_PAGE_H

#include "blockMapEntry.h"
#include "header.h"
#include "types.h"

enum {
  /**
   * The offset used to store PBNs in pre-Sodium block map pages. Used when
   * upgrading the header format of block map pages.
   **/
  NEON_BLOCK_MAP_ENTRY_PBN_OFFSET = 1,
};

/**
 * The pre-sodium block map page header.
 **/
typedef struct __attribute__((packed)) {
  /**
   * The nonce of the current VDO, used to determine whether or not a page has
   * been formatted.
   **/
  uint64_t       nonce;

  /** The number of the page, used for consistency checking */
  PageNumber     pageID;

  /**
   * The earliest journal block that contains uncommitted updates to this
   * page. This field is meaningless (and out of date) on disk, but written
   * for convenience.
   **/
  SequenceNumber recoverySequenceNumber;

  /** If this field is non-zero, this page has not yet been initialized. */
  uint64_t       uninitialized;
} PageHeader4_0;

/**
 * The block map page header.
 **/
typedef struct __attribute__((packed)) {
  /**
   * The nonce of the current VDO, used to determine whether or not a page has
   * been formatted.
   **/
  uint64_t            nonce;

  /** The PBN of the page */
  PhysicalBlockNumber pbn;

  /**
   * The earliest journal block that contains uncommitted updates to this
   * page. This field is meaningless (and out of date) on disk, but written
   * for convenience.
   **/
  SequenceNumber      recoverySequenceNumber;

  /** Whether this page has been initialized on disk (i.e. written twice). */
  bool                initialized;

  /**
   * The (positive) offset that the PBNs in the entries on this page have
   * relative to absolute PBNs.
   **/
  uint8_t             entryOffset;

  /**
   * Whether this page is an interior tree page being written out. This field
   * is meaningless on disk, but written for convenience.
   **/
  bool                interiorTreePageWriting;

  /**
   * If this is a dirty tree page, the tree zone flush generation in which it
   * was last dirtied. This field is meaningless on disk, but written for
   * convenience. If this is not a dirty tree page, this field is meaningless.
   */
  uint8_t             generation;
} PageHeader;

/**
 * The format of a block map page.
 **/
typedef struct __attribute__((packed)) {
  VersionNumber version;
  PageHeader    header;
  BlockMapEntry entries[];
} BlockMapPage;

typedef enum {
  // A block map page is correctly initialized
  BLOCK_MAP_PAGE_VALID,
  // A block map page is uninitialized
  BLOCK_MAP_PAGE_INVALID,
  // A block map page is intialized, but is the wrong page
  BLOCK_MAP_PAGE_BAD,
} BlockMapPageValidity;

/**
 * Check whether a block map page is of the current version.
 *
 * @param page  The page to check
 *
 * @return <code>true</code> if the page has the current version
 **/
bool isCurrentBlockMapPage(const BlockMapPage *page)
  __attribute__((warn_unused_result));

/**
 * Encode a block map page entry.
 *
 * @param page  The page on which to encode the entry
 * @param slot  The slot in which to encode the entry
 * @param pbn   The absolute PhysicalBlockNumber of the entry to encode
 * @param state The mapping state of the entry to encode
 **/
void encodeBlockMapEntry(BlockMapPage        *page,
                         SlotNumber           slot,
                         PhysicalBlockNumber  pbn,
                         BlockMappingState    state);

/**
 * Format a block map page in memory.
 *
 * @param buffer  The buffer which holds the page
 * @param nonce   The VDO nonce
 * @param pbn     The absolute pbn of the page
 **/
void formatBlockMapPage(void *buffer, Nonce nonce, PhysicalBlockNumber pbn);

/**
 * Check whether a newly read page is valid, upgrading its in-memory format if
 * possible and necessary. If the page is valid, clear fields which are not
 * meaningful on disk.
 *
 * @param page   The page to validate
 * @param nonce  The VDO nonce
 * @param pbn    The expected absolute PBN of the page
 *
 * @return The validity of the page
 **/
BlockMapPageValidity validateBlockMapPage(BlockMapPage        *page,
                                          Nonce                nonce,
                                          PhysicalBlockNumber  pbn)
  __attribute__((warn_unused_result));

/**
 * Update an entry on a block map page.
 *
 * @param dataVIO       The DataVIO making the update
 * @param page          The page to update
 * @param pbn           The new PBN for the entry
 * @param mappingState  The new mapping state for the entry
 **/
void updateBlockMapPage(DataVIO             *dataVIO,
                        BlockMapPage        *page,
                        PhysicalBlockNumber  pbn,
                        BlockMappingState    mappingState);

#endif // BLOCK_MAP_PAGE_H
