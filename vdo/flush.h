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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/flush.h#10 $
 */

#ifndef FLUSH_H
#define FLUSH_H

#include "types.h"
#include "waitQueue.h"

/**
 * A marker for tracking which journal entries are affected by a flush request.
 **/
struct vdo_flush {
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
int __must_check make_flusher(struct vdo *vdo);

/**
 * Free a flusher and null out the reference to it.
 *
 * @param flusher_ptr  A pointer to the flusher to free
 **/
void free_flusher(struct flusher **flusher_ptr);

/**
 * Get the ID of the thread on which flusher functions should be called.
 *
 * @param flusher  The flusher to query
 *
 * @return The ID of the thread which handles the flusher
 **/
thread_id_t __must_check get_flusher_thread_id(struct flusher *flusher);

/**
 * Handle empty flush requests.
 *
 * @param vdo        The vdo
 * @param vdo_flush  The opaque flush request
 **/
void flush(struct vdo *vdo, struct vdo_flush *vdo_flush);

/**
 * Attempt to complete any flushes which might have finished.
 *
 * @param flusher  The flusher
 **/
void complete_flushes(struct flusher *flusher);

/**
 * Dump the flusher, in a thread-unsafe fashion.
 *
 * @param flusher  The flusher
 **/
void dump_flusher(const struct flusher *flusher);

/**
 * Complete and free a vdo flush request.
 *
 * @param flush_ptr  The pointer to the flush reference, which will be nulled
 **/
void vdo_complete_flush(struct vdo_flush **flush_ptr);

#endif /* FLUSH_H */
