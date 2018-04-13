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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/flush.h#1 $
 */

#ifndef FLUSH_H
#define FLUSH_H

#include "types.h"
#include "waitQueue.h"

/**
 * A marker for tracking which journal entries are affected by a flush request.
 **/
struct vdoFlush {
  /** The wait queue entry for this flush */
  Waiter         waiter;
  /** Which flush this struct represents */
  SequenceNumber flushGeneration;
};

/**
 * Make a flusher for a VDO.
 *
 * @param vdo  The VDO which owns the flusher
 *
 * @return VDO_SUCCESS or an error
 **/
int makeFlusher(VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Free a flusher and null out the reference to it.
 *
 * @param flusherPtr  A pointer to the flusher to free
 **/
void freeFlusher(Flusher **flusherPtr);

/**
 * Get the ID of the thread on which flusher functions should be called.
 *
 * @param flusher  The flusher to query
 *
 * @return The ID of the thread which handles the flusher
 **/
ThreadID getFlusherThreadID(Flusher *flusher)
  __attribute__((warn_unused_result));

/**
 * Handle empty flush requests.
 *
 * @param vdo       The VDO
 * @param vdoFlush  The opaque flush request
 **/
void flush(VDO *vdo, VDOFlush *vdoFlush);

/**
 * Attempt to complete any flushes which might have finished.
 *
 * @param flusher  The flusher
 **/
void completeFlushes(Flusher *flusher);

/**
 * Dump the flusher, in a thread-unsafe fashion.
 *
 * @param flusher  The flusher
 **/
void dumpFlusher(const Flusher *flusher);

#endif /* FLUSH_H */
