/*
 * Copyright Red Hat
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
 * $Id: //eng/uds-releases/krusty/src/uds/indexCheckpoint.h#6 $
 */

#ifndef INDEX_CHECKPOINT_H
#define INDEX_CHECKPOINT_H

#include "index.h"

/**
 * Construct and initialize the checkpoint sub-structure of an index.
 *
 * @param index  the index receive the new checkpoint structure.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_index_checkpoint(struct uds_index *index);

/**
 * Free the checkpoint sub-structure of an index.
 *
 * @param checkpoint  the structure to free
 **/
void free_index_checkpoint(struct index_checkpoint *checkpoint);

/**
 * Get the current checkpointing frequency of an index.
 *
 * @param checkpoint  the checkpoint state of the index
 *
 * @return the number of chapters between checkpoints
 **/
unsigned int __must_check
get_index_checkpoint_frequency(struct index_checkpoint *checkpoint);

/**
 * Set checkpointing frequency for the index.
 *
 * @param checkpoint  the checkpoint state of the index
 * @param frequency   The new checkpointing frequency
 *
 * @return the old checkpointing frequency
 **/
unsigned int
set_index_checkpoint_frequency(struct index_checkpoint *checkpoint,
			       unsigned int frequency);

/**
 * Gets the number of checkpoints completed during the lifetime of this index
 *
 * @param checkpoint  the checkpoint state of the index
 *
 * @return            the number of checkpoints completed
 **/
uint64_t __must_check
get_checkpoint_count(struct index_checkpoint *checkpoint);

/**
 * If incremental checkpointing is in progress, finish it.
 *
 * @param index     The index
 *
 * @return          UDS_SUCCESS or an error code
 *
 * @note        This function is called automatically during normal operation;
 *              its presence here is for tests that expect checkpointing to
 *              have completed at some point in their logic.  It is not an
 *              error to call this function if checkpointing is not in
 *              progress, it silently returns success.
 **/
int __must_check finish_checkpointing(struct uds_index *index);

/**
 * Process one zone's incremental checkpoint operation. Automatically
 * starts, processes, and finishes a checkpoint over multiple invocations
 * as successive chapters are closed and written.
 *
 * Uses its own mutex to serialize the starting and finishing or aborting,
 * but allows parallel execution of the incremental progress.
 *
 * @param index                The index to checkpoint
 * @param zone                 The current zone number
 * @param new_virtual_chapter  The number of the chapter which the calling
 *                             zone has just opened
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check process_checkpointing(struct uds_index *index,
				       unsigned int zone,
				       uint64_t new_virtual_chapter);

/**
 * Process saves done outside any zone by the chapter writer.
 *
 * Grabs the mutex associated with process_checkpointing().
 *
 * @param index         The index to process.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
process_chapter_writer_checkpoint_saves(struct uds_index *index);

#endif // INDEX_CHECKPOINT_H
