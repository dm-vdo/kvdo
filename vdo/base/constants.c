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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/constants.c#1 $
 */

#include "types.h"

/** The maximum logical space is 4 petabytes, which is 1 terablock. */
const BlockCount MAXIMUM_LOGICAL_BLOCKS  = 1024ULL * 1024 * 1024 * 1024;

/** The maximum physical space is 256 terabytes, which is 64 gigablocks. */
const BlockCount MAXIMUM_PHYSICAL_BLOCKS = 1024ULL * 1024 * 1024 * 64;

// unit test minimum
const BlockCount MINIMUM_SLAB_JOURNAL_BLOCKS = 2;
