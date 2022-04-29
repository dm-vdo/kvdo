/* SPDX-License-Identifier: GPL-2.0-only */
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
 */

#ifndef VDO_RECOVERY_H
#define VDO_RECOVERY_H

#include "completion.h"
#include "vdo.h"

void vdo_replay_into_slab_journals(struct block_allocator *allocator,
				   struct vdo_completion *completion,
				   void *context);

void vdo_launch_recovery(struct vdo *vdo, struct vdo_completion *parent);

#endif /* VDO_RECOVERY_H */
