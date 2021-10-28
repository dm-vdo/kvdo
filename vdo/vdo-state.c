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

#include "vdo-state.h"

#include "permassert.h"


/**********************************************************************/
const char *describe_vdo_state(enum vdo_state state)
{
	// These strings should all fit in the 15 chars of VDOStatistics.mode.
	switch (state) {
	case VDO_RECOVERING:
		return "recovering";

	case VDO_READ_ONLY_MODE:
		return "read-only";

	default:
		return "normal";
	}
}
