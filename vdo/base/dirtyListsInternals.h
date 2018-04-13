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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/dirtyListsInternals.h#1 $
 */

#ifndef DIRTY_LISTS_INTERNALS_H
#define DIRTY_LISTS_INTERNALS_H

#include "dirtyLists.h"
#include "types.h"

/**
 * Get the next period from a DirtyLists. This method is used by unit tests.
 *
 * @param dirtyLists  The DirtyLists to examine
 **/
SequenceNumber getDirtyListsNextPeriod(DirtyLists *dirtyLists)
  __attribute__((warn_unused_result));

#endif // DIRTY_LISTS_INTERNALS_H
