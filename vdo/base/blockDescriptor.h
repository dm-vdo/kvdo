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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockDescriptor.h#1 $
 */

#ifndef BLOCK_DESCRIPTOR_H
#define BLOCK_DESCRIPTOR_H

#include "permassert.h"

#include "ringNode.h"
#include "types.h"

typedef struct {
  RingNode           ringNode;       // The ring node for waiting
  SlabJournal       *journal;        // The slab journal
  SequenceNumber     sequenceNumber; // Sequence number for the block
} BlockDescriptor;

/**
 * Convert a ring node to a BlockDescriptor.
 *
 * @param node The ring node to recast as a block descriptor
 *
 * @return the block descriptor
 **/
__attribute__((warn_unused_result))
static inline BlockDescriptor *asBlockDescriptor(RingNode *node)
{
  STATIC_ASSERT(offsetof(BlockDescriptor, ringNode) == 0);
  return (BlockDescriptor *) node;
}

#endif // BLOCK_DESCRIPTOR_H
