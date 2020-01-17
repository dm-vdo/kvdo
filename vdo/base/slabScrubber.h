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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabScrubber.h#9 $
 */

#ifndef SLAB_SCRUBBER_H
#define SLAB_SCRUBBER_H

#include "completion.h"
#include "types.h"
#include "waitQueue.h"

/**
 * Create a slab scrubber
 *
 * @param layer             The physical layer of the VDO
 * @param slabJournalSize   The size of a slab journal in blocks
 * @param readOnlyNotifier  The context for entering read-only mode
 * @param scrubberPtr       A pointer to hold the scrubber
 *
 * @return VDO_SUCCESS or an error
 **/
int makeSlabScrubber(PhysicalLayer              *layer,
                     BlockCount                  slabJournalSize,
                     struct read_only_notifier  *readOnlyNotifier,
                     struct slab_scrubber      **scrubberPtr)
  __attribute__((warn_unused_result));

/**
 * Free a slab scrubber and null out the reference to it.
 *
 * @param scrubberPtr  A pointer to the scrubber to destroy
 **/
void freeSlabScrubber(struct slab_scrubber **scrubberPtr);

/**
 * Check whether a scrubber has slabs to scrub.
 *
 * @param scrubber  The scrubber to check
 *
 * @return <code>true</code> if the scrubber has slabs to scrub
 **/
bool hasSlabsToScrub(struct slab_scrubber *scrubber)
  __attribute__((warn_unused_result));

/**
 * Register a slab with a scrubber.
 *
 * @param scrubber      The scrubber
 * @param slab          The slab to scrub
 * @param highPriority  <code>true</code> if the slab should be put on the
 *                      high-priority queue
 **/
void registerSlabForScrubbing(struct slab_scrubber *scrubber,
                              struct vdo_slab      *slab,
                              bool                  highPriority);

/**
 * Scrub all the slabs which have been registered with a slab scrubber.
 *
 * @param scrubber      The scrubber
 * @param parent        The object to notify when scrubbing is complete
 * @param callback      The function to run when scrubbing is complete
 * @param errorHandler  The handler for scrubbing errors
 **/
void scrubSlabs(struct slab_scrubber *scrubber,
                void                 *parent,
                VDOAction            *callback,
                VDOAction            *errorHandler);

/**
 * Scrub any slabs which have been registered at high priority with a slab
 * scrubber.
 *
 * @param scrubber         The scrubber
 * @param scrubAtLeastOne  <code>true</code> if one slab should always be
 *                         scrubbed, even if there are no high-priority slabs
 *                         (and there is at least one low priority slab)
 * @param parent           The completion to notify when scrubbing is complete
 * @param callback         The function to run when scrubbing is complete
 * @param errorHandler     The handler for scrubbing errors
 **/
void scrubHighPrioritySlabs(struct slab_scrubber  *scrubber,
                            bool                   scrubAtLeastOne,
                            struct vdo_completion *parent,
                            VDOAction             *callback,
                            VDOAction             *errorHandler);

/**
 * Tell the scrubber to stop scrubbing after it finishes the slab it is
 * currently working on.
 *
 * @param scrubber  The scrubber to stop
 * @param parent    The completion to notify when scrubbing has stopped
 **/
void stopScrubbing(struct slab_scrubber  *scrubber,
                   struct vdo_completion *parent);

/**
 * Tell the scrubber to resume scrubbing if it has been stopped.
 *
 * @param scrubber  The scrubber to resume
 * @param parent    The object to notify once scrubbing has resumed
 **/
void resumeScrubbing(struct slab_scrubber  *scrubber,
                     struct vdo_completion *parent);

/**
 * Wait for a clean slab.
 *
 * @param scrubber  The scrubber on which to wait
 * @param waiter    The waiter
 *
 * @return VDO_SUCCESS if the waiter was queued, VDO_NO_SPACE if there are no
 *         slabs to scrub, and some other error otherwise
 **/
int enqueueCleanSlabWaiter(struct slab_scrubber *scrubber,
                           struct waiter        *waiter);

/**
 * Get the number of slabs that are unrecovered or being scrubbed.
 *
 * @param scrubber  The scrubber to query
 *
 * @return the number of slabs that are unrecovered or being scrubbed
 **/
SlabCount getScrubberSlabCount(const struct slab_scrubber *scrubber)
  __attribute__((warn_unused_result));

/**
 * Dump information about a slab scrubber to the log for debugging.
 *
 * @param scrubber   The scrubber to dump
 **/
void dumpSlabScrubber(const struct slab_scrubber *scrubber);

#endif /* SLAB_SCRUBBER_H */
