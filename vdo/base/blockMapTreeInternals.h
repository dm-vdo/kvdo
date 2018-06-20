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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMapTreeInternals.h#3 $
 */

#ifndef BLOCK_MAP_TREE_INTERNALS_H
#define BLOCK_MAP_TREE_INTERNALS_H

#include "blockMapTree.h"

#include "blockMapPage.h"
#include "types.h"

/** A single page of a block map tree */
struct treePage {
  /** Waiter for a VIO to write out this page */
  Waiter         waiter;

  /** Dirty list node */
  RingNode       node;

  /**
   * If this is a dirty tree page, the tree zone flush generation in which it
   * was last dirtied.
   */
  uint8_t        generation;

  /** Whether this page is an interior tree page being written out. */
  bool           writing;

  /**
   * If this page is being written, the tree zone flush generation of the
   * copy of the page being written.
   **/
  uint8_t        writingGeneration;

  /** The earliest journal block containing uncommitted updates to this page */
  SequenceNumber recoveryLock;

  /** The value of recoveryLock when the this page last started writing */
  SequenceNumber writingRecoveryLock;

  /** The buffer to hold the on-disk representation of this page */
  char           pageBuffer[VDO_BLOCK_SIZE];
};

typedef struct {
  TreePage *levels[BLOCK_MAP_TREE_HEIGHT];
} BlockMapTreeSegment;

struct blockMapTree {
  BlockMapTreeSegment *segments;
};

typedef struct {
  PageNumber levels[BLOCK_MAP_TREE_HEIGHT];
} Boundary;

/**
 * An invalid PBN used to indicate that the page holding the location of a
 * tree root has been "loaded".
 **/
extern const PhysicalBlockNumber INVALID_PBN;

/**
 * Extract the BlockMapPage from a TreePage.
 *
 * @param treePage  The TreePage
 *
 * @return The BlockMapPage of the TreePage
 **/
__attribute__((warn_unused_result))
static inline BlockMapPage *asBlockMapPage(TreePage *treePage)
{
  return (BlockMapPage *) treePage->pageBuffer;
}

/**
 * Replace the VIOPool in a tree zone. This method is used by unit tests.
 *
 * @param zone      The zone whose pool is to be replaced
 * @param layer     The physical layer from which to make VIOs
 * @param poolSize  The size of the new pool
 *
 * @return VDO_SUCCESS or an error
 **/
int replaceTreeZoneVIOPool(BlockMapTreeZone *zone,
                           PhysicalLayer    *layer,
                           size_t            poolSize)
  __attribute__((warn_unused_result));

/**
 * Check whether a buffer contains a valid page. If the page is bad, log an
 * error. If the page is valid, copy it to the supplied page.
 *
 * @param buffer  The buffer to validate (and copy)
 * @param nonce   The VDO nonce
 * @param pbn     The absolute PBN of the page
 * @param page    The page to copy into if valid
 *
 * @return <code>true</code> if the page was copied (valid)
 **/
bool copyValidPage(char                *buffer,
                   Nonce                nonce,
                   PhysicalBlockNumber  pbn,
                   BlockMapPage        *page);

#endif // BLOCK_MAP_TREE_INTERNALS_H
