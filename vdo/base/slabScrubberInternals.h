/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabScrubberInternals.h#11 $
 */

#ifndef SLAB_SCRUBBER_INTERNALS_H
#define SLAB_SCRUBBER_INTERNALS_H

#include "slabScrubber.h"

#include "adminState.h"
#include "atomic.h"
#include "extent.h"
#include "ringNode.h"

struct slab_scrubber {
  struct vdo_completion      completion;
  /** The queue of slabs to scrub first */
  RingNode                   highPrioritySlabs;
  /** The queue of slabs to scrub once there are no highPrioritySlabs */
  RingNode                   slabs;
  /** The queue of VIOs waiting for a slab to be scrubbed */
  struct wait_queue          waiters;

  // The number of slabs that are unrecovered or being scrubbed. This field is
  // modified by the physical zone thread, but is queried by other threads.
  Atomic64                   slabCount;

  /** The administrative state of the scrubber */
  struct admin_state         adminState;
  /** Whether to only scrub high-priority slabs */
  bool                       highPriorityOnly;
  /** The context for entering read-only mode */
  struct read_only_notifier *readOnlyNotifier;
  /** The slab currently being scrubbed */
  struct vdo_slab           *slab;
  /** The extent for loading slab journal blocks */
  struct vdo_extent         *extent;
  /** A buffer to store the slab journal blocks */
  char                      *journalData;
};

#endif // SLAB_SCRUBBER_INTERNALS_H
