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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabCompletion.h#5 $
 */
#ifndef SLAB_COMPLETION_H
#define SLAB_COMPLETION_H

#include "completion.h"
#include "slabIterator.h"
#include "types.h"

/**
 * Allocate a completion for loading, saving or flushing a slab.
 *
 * @param [in]  layer                 The layer for the completion
 * @param [out] completionPtr         A pointer to hold the new completion
 *
 * @return VDO_SUCCESS or an error
 **/
int makeSlabCompletion(PhysicalLayer *layer, VDOCompletion **completionPtr)
  __attribute__((warn_unused_result));

/**
 * Free a SlabCompletion and null out the reference to it.
 *
 * @param completionPtr  The reference to the completion to free
 **/
void freeSlabCompletion(VDOCompletion **completionPtr);

/**
 * Flush slab journals.
 *
 * @param completion    The completion for the flush operation
 * @param slabIterator  An iterator over the slabs having journals to flush
 **/
void flushSlabJournals(VDOCompletion *completion, SlabIterator slabIterator);

/**
 * Save fully rebuilt slabs.
 *
 * @param completion    The completion for the save operation
 * @param slabIterator  An iterator over the slabs to save
 **/
void saveFullyRebuiltSlabs(VDOCompletion *completion,
                           SlabIterator   slabIterator);

#endif /* SLAB_COMPLETION_H */
