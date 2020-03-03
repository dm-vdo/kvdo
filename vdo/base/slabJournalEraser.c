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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabJournalEraser.c#12 $
 */

#include "slabJournalEraser.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "constants.h"
#include "extent.h"
#include "slab.h"
#include "slabDepot.h"

struct slab_journal_eraser {
	struct vdo_completion *parent;
	struct vdo_extent *extent;
	char *zero_buffer;
	struct slab_iterator slabs;
};

/**
 * Free the eraser and finish the parent.
 *
 * @param eraser    The eraser that is done
 * @param result    The result to return to the parent
 **/
static void finish_erasing(struct slab_journal_eraser *eraser, int result)
{
	struct vdo_completion *parent = eraser->parent;
	free_extent(&eraser->extent);
	FREE(eraser->zero_buffer);
	FREE(eraser);
	finishCompletion(parent, result);
}

/**
 * Finish erasing slab journals with an error.
 *
 * @param completion   A completion whose parent is the eraser
 **/
static void handle_erasing_error(struct vdo_completion *completion)
{
	struct slab_journal_eraser *eraser = completion->parent;
	finish_erasing(eraser, eraser->extent->completion.result);
}

/**
 * Erase the next slab journal.
 *
 * @param extentCompletion  A completion whose parent is the eraser
 **/
static void erase_next_slab_journal(struct vdo_completion *extentCompletion)
{
	struct slab_journal_eraser *eraser = extentCompletion->parent;

	if (!has_next_slab(&eraser->slabs)) {
		finish_erasing(eraser, VDO_SUCCESS);
		return;
	}

	struct vdo_slab *slab = next_slab(&eraser->slabs);
	write_metadata_extent(eraser->extent, slab->journal_origin);
}

/**********************************************************************/
void erase_slab_journals(struct slab_depot *depot,
			 struct slab_iterator slabs,
			 struct vdo_completion *parent)
{
	struct slab_journal_eraser *eraser;
	int result = ALLOCATE(1, struct slab_journal_eraser, __func__, &eraser);
	if (result != VDO_SUCCESS) {
		finishCompletion(parent, result);
		return;
	}

	eraser->parent = parent;
	eraser->slabs = slabs;

	BlockCount journalSize = get_slab_config(depot)->slabJournalBlocks;
	result = ALLOCATE(journalSize * VDO_BLOCK_SIZE,
			  char,
			  __func__,
			  &eraser->zero_buffer);
	if (result != VDO_SUCCESS) {
		finish_erasing(eraser, result);
		return;
	}

	result = create_extent(parent->layer,
			       VIO_TYPE_SLAB_JOURNAL,
			       VIO_PRIORITY_METADATA,
			       journalSize,
			       eraser->zero_buffer,
			       &eraser->extent);
	if (result != VDO_SUCCESS) {
		finish_erasing(eraser, result);
		return;
	}

	struct vdo_completion *extent_completion = &eraser->extent->completion;
	prepareCompletion(extent_completion,
			  erase_next_slab_journal,
			  handle_erasing_error,
			  getCallbackThreadID(),
			  eraser);
	erase_next_slab_journal(extent_completion);
}
