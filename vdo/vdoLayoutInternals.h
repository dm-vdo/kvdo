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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/vdoLayoutInternals.h#1 $
 */

#ifndef VDO_LAYOUT_INTERNALS_H
#define VDO_LAYOUT_INTERNALS_H

#include "fixedLayout.h"
#include "types.h"

struct vdo_layout {
	// The current layout of the VDO
	struct fixed_layout *layout;
	// The next layout of the VDO
	struct fixed_layout *next_layout;
	// The previous layout of the VDO
	struct fixed_layout *previous_layout;
	// The first block in the layouts
	physical_block_number_t starting_offset;
	// A pointer to the copy completion (if there is one)
	struct vdo_completion *copy_completion;
};

#endif // VDO_LAYOUT_INTERNALS_H
