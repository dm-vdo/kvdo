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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoRecovery.h#6 $
 */

#ifndef VDO_RECOVERY_H
#define VDO_RECOVERY_H

#include "completion.h"
#include "vdo.h"

/**
 * Replay recovery journal entries in the the slab journals of slabs owned by a
 * given block_allocator.
 *
 * @param allocator   The allocator whose slab journals are to be recovered
 * @param completion  The completion to use for waiting on slab journal space
 * @param context     The slab depot load context supplied by a recovery when
 *                    it loads the depot
 **/
void replay_into_slab_journals(struct block_allocator *allocator,
			       struct vdo_completion *completion,
			       void *context);

/**
 * Construct a recovery completion and launch it. Apply all valid journal block
 * entries to all vdo structures. This function performs the offline portion of
 * recovering a vdo from a crash.
 *
 * @param vdo     The vdo to recover
 * @param parent  The completion to notify when the offline portion of the
 *                recovery is complete
 **/
void launch_recovery(struct vdo *vdo, struct vdo_completion *parent);

#endif // VDO_RECOVERY_H
