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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabScrubberInternals.h#2 $
 */

#ifndef SLAB_SCRUBBER_INTERNALS_H
#define SLAB_SCRUBBER_INTERNALS_H

#include "slabScrubber.h"

#include "atomic.h"
#include "ringNode.h"

struct slabScrubber {
  VDOCompletion                  completion;
  /** The queue of slabs to scrub first */
  RingNode                       highPrioritySlabs;
  /** The queue of slabs to scrub once there are no highPrioritySlabs */
  RingNode                       slabs;
  /** The queue of VIOs waiting for a slab to be scrubbed */
  WaitQueue                      waiters;

  // The number of slabs that are unrecovered or being scrubbed. This field is
  // modified by the physical zone thread, but is queried by other threads.
  Atomic64                       slabCount;

  /** Whether the scrubber is actively scrubbing */
  bool                           isScrubbing;
  /** Whether the scrubber has been asked to stop scrubbing */
  bool                           stopScrubbing;
  /** Whether to only scrub high-priority slabs */
  bool                           highPriorityOnly;
  /** The completion for rebuilding a slab */
  VDOCompletion                 *slabRebuildCompletion;
  /** The context for entering read-only mode */
  ReadOnlyModeContext           *readOnlyContext;
};

#endif // SLAB_SCRUBBER_INTERNALS_H
