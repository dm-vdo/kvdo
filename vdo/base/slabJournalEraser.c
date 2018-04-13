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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabJournalEraser.c#1 $
 */

#include "slabJournalEraser.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "constants.h"
#include "extent.h"
#include "slab.h"
#include "slabDepot.h"

typedef struct {
  VDOCompletion *parent;
  VDOExtent     *extent;
  char          *zeroBuffer;
  SlabIterator   slabs;
} SlabJournalEraser;

/**
 * Free the eraser and finish the parent.
 *
 * @param eraser    The eraser that is done
 * @param result    The result to return to the parent
 **/
static void finishErasing(SlabJournalEraser *eraser, int result)
{
  VDOCompletion *parent = eraser->parent;
  freeExtent(&eraser->extent);
  FREE(eraser->zeroBuffer);
  FREE(eraser);
  finishCompletion(parent, result);
}

/**
 * Finish erasing slab journals with an error.
 *
 * @param completion   A completion whose parent is the eraser
 **/
static void handleErasingError(VDOCompletion *completion)
{
  SlabJournalEraser *eraser = completion->parent;
  finishErasing(eraser, eraser->extent->completion.result);
}

/**
 * Erase the next slab journal.
 *
 * @param extentCompletion  A completion whose parent is the eraser
 **/
static void eraseNextSlabJournal(VDOCompletion *extentCompletion)
{
  SlabJournalEraser *eraser = extentCompletion->parent;

  if (!hasNextSlab(&eraser->slabs)) {
    finishErasing(eraser, VDO_SUCCESS);
    return;
  }

  Slab *slab = nextSlab(&eraser->slabs);
  writeMetadataExtent(eraser->extent, slab->journalOrigin);
}

/**********************************************************************/
void eraseSlabJournals(SlabDepot     *depot,
                       SlabIterator   slabs,
                       VDOCompletion *parent)
{
  SlabJournalEraser *eraser;
  int result = ALLOCATE(1, SlabJournalEraser, __func__, &eraser);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  eraser->parent = parent;
  eraser->slabs  = slabs;

  BlockCount journalSize = getSlabConfig(depot)->slabJournalBlocks;
  result = ALLOCATE(journalSize * VDO_BLOCK_SIZE, char, __func__,
                    &eraser->zeroBuffer);
  if (result != VDO_SUCCESS) {
    finishErasing(eraser, result);
    return;
  }

  result = createExtent(parent->layer, VIO_TYPE_SLAB_JOURNAL,
                        VIO_PRIORITY_METADATA, journalSize, eraser->zeroBuffer,
                        &eraser->extent);
  if (result != VDO_SUCCESS) {
    finishErasing(eraser, result);
    return;
  }

  VDOCompletion *extentCompletion = &eraser->extent->completion;
  prepareCompletion(extentCompletion, eraseNextSlabJournal,
                    handleErasingError, getCallbackThreadID(), eraser);
  eraseNextSlabJournal(extentCompletion);
}
