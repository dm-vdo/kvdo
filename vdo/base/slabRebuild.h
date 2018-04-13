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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabRebuild.h#1 $
 */

#ifndef SLAB_REBUILD_H
#define SLAB_REBUILD_H

#include "slab.h"

/**
 * Allocate a slab rebuild completion.
 *
 * @param [in]  layer             The physical layer
 * @param [in]  slabJournalSize   The size of a slab journal in blocks
 * @param [out] completionPtr     The pointer to hold the new completion
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeSlabRebuildCompletion(PhysicalLayer  *layer,
                              BlockCount      slabJournalSize,
                              VDOCompletion **completionPtr)
  __attribute__((warn_unused_result));

/**
 * Free a slab rebuild completion and NULL the reference.
 *
 * @param completionPtr  A pointer to the slab rebuild completion to free
 **/
void freeSlabRebuildCompletion(VDOCompletion **completionPtr);

/**
 * Scrub a slab by applying all slab journal entries to the reference counts.
 *
 * @param slab             The slab to scrub
 * @param completion       The slab rebuild completion
 **/
void scrubSlab(Slab *slab, VDOCompletion *completion);

#endif // SLAB_REBUILD_H
