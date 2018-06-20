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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMapPage.h#6 $
 */

#ifndef BLOCK_MAP_PAGE_H
#define BLOCK_MAP_PAGE_H

#include "blockMapEntry.h"
#include "header.h"
#include "types.h"

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

  /** Formerly recoverySequenceNumber; may be non-zero on disk */
  uint64_t            unusedLongWord;

  /** Whether this page has been initialized on disk (i.e. written twice). */
  bool                initialized;

  /** Formerly entryOffset; now unused since it should always be zero */
  uint8_t             unusedByte1;

  /** Formerly interiorTreePageWriting; may be non-zero on disk */
  uint8_t             unusedByte2;

  /** Formerly generation (for dirty tree pages); may be non-zero on disk */
  uint8_t             unusedByte3;
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
 * Check whether a block map page has been initialized.
 *
 * @param page  The page to check
 *
 * @return <code>true</code> if the page has been initialized
 **/
__attribute__((warn_unused_result))
static inline bool isBlockMapPageInitialized(const BlockMapPage *page)
{
  return page->header.initialized;
}

/**
 * Mark whether a block map page has been initialized.
 *
 * @param page         The page to mark
 * @param initialized  The state to set
 *
 * @return <code>true</code> if the initialized flag was modified
 **/
static inline bool markBlockMapPageInitialized(BlockMapPage *page,
                                               bool          initialized)
{
  if (initialized == page->header.initialized) {
    return false;
  }

  page->header.initialized = initialized;
  return true;
}

/**
 * Get the physical block number where a block map page is stored.
 *
 * @param page  The page to query
 *
 * @return the page's physical block number
 **/
__attribute__((warn_unused_result))
static inline PhysicalBlockNumber getBlockMapPagePBN(const BlockMapPage *page)
{
  return page->header.pbn;
}

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
 * Format a block map page in memory.
 *
 * @param buffer       The buffer which holds the page
 * @param nonce        The VDO nonce
 * @param pbn          The absolute PBN of the page
 * @param initialized  Whether the page should be marked as initialized
 *
 * @return the buffer pointer, as a block map page (for convenience)
 **/
BlockMapPage *formatBlockMapPage(void                *buffer,
                                 Nonce                nonce,
                                 PhysicalBlockNumber  pbn,
                                 bool                 initialized);

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
 * @param [in]     page          The page to update
 * @param [in]     dataVIO       The DataVIO making the update
 * @param [in]     pbn           The new PBN for the entry
 * @param [in]     mappingState  The new mapping state for the entry
 * @param [in,out] recoveryLock  A reference to the current recovery sequence
 *                               number lock held by the page. Will be updated
 *                               if the lock changes to protect the new entry
 **/
void updateBlockMapPage(BlockMapPage        *page,
                        DataVIO             *dataVIO,
                        PhysicalBlockNumber  pbn,
                        BlockMappingState    mappingState,
                        SequenceNumber      *recoveryLock);

#endif // BLOCK_MAP_PAGE_H
