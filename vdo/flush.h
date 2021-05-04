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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/flush.h#1 $
 */

#ifndef FLUSH_H
#define FLUSH_H

#include "bio.h"
#include "workQueue.h"

#include "types.h"
#include "waitQueue.h"

/**
 * A marker for tracking which journal entries are affected by a flush request.
 **/
struct vdo_flush {
	/** The work item for enqueueing this flush request. */
	struct vdo_work_item work_item;
	/** The vdo to flush */
	struct vdo *vdo;
	/** The flush bios covered by this request */
	struct bio_list bios;
	/** Time when the earlier bio arrived */
	uint64_t arrival_jiffies;
	/** The wait queue entry for this flush */
	struct waiter waiter;
	/** Which flush this struct represents */
	sequence_number_t flush_generation;
};

/**
 * Make a flusher for a vdo.
 *
 * @param vdo  The vdo which owns the flusher
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check make_vdo_flusher(struct vdo *vdo);

/**
 * Free a flusher and null out the reference to it.
 *
 * @param flusher_ptr  A pointer to the flusher to free
 **/
void free_vdo_flusher(struct flusher **flusher_ptr);

/**
 * Get the ID of the thread on which flusher functions should be called.
 *
 * @param flusher  The flusher to query
 *
 * @return The ID of the thread which handles the flusher
 **/
thread_id_t __must_check get_vdo_flusher_thread_id(struct flusher *flusher);

/**
 * Handle empty flush requests.
 *
 * @param item  A flush request (as a work_item)
 **/
void flush_vdo(struct vdo_work_item *item);

/**
 * Attempt to complete any flushes which might have finished.
 *
 * @param flusher  The flusher
 **/
void complete_vdo_flushes(struct flusher *flusher);

/**
 * Dump the flusher, in a thread-unsafe fashion.
 *
 * @param flusher  The flusher
 **/
void dump_vdo_flusher(const struct flusher *flusher);

/**
 * Complete and free a vdo flush request.
 *
 * @param flush_ptr  The pointer to the flush reference, which will be nulled
 **/
void vdo_complete_flush(struct vdo_flush **flush_ptr);

/**
 * Function called to start processing a flush request. It is called when we
 * receive an empty flush bio from the block layer, and before acknowledging a
 * non-empty bio with the FUA flag set.
 *
 * @param vdo  The vdo
 * @param bio  The bio containing an empty flush request
 **/
void launch_vdo_flush(struct vdo *vdo, struct bio *bio);

#endif /* FLUSH_H */
