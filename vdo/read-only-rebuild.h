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

#ifndef READ_ONLY_REBUILD_H
#define READ_ONLY_REBUILD_H

#include "completion.h"
#include "vdo.h"

/**
 * Construct a read_only_rebuild_completion and launch it. Apply all valid
 * journal block entries to all vdo structures. Must be launched from logical
 * zone 0.
 *
 * @param vdo           The vdo to rebuild
 * @param parent        The completion to notify when the rebuild is complete
 **/
void launch_vdo_rebuild(struct vdo *vdo, struct vdo_completion *parent);

#endif /* READ_ONLY_REBUILD_H */
