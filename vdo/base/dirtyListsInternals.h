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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/dirtyListsInternals.h#4 $
 */

#ifndef DIRTY_LISTS_INTERNALS_H
#define DIRTY_LISTS_INTERNALS_H

#include "dirtyLists.h"
#include "types.h"

/**
 * Get the next period from a dirty_lists structure. This method is
 * used by unit tests.
 *
 * @param dirty_lists  The dirty_lists to examine
 **/
sequence_number_t get_dirty_lists_next_period(struct dirty_lists *dirty_lists)
	__attribute__((warn_unused_result));

#endif // DIRTY_LISTS_INTERNALS_H
