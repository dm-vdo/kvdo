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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/referenceCountRebuild.h#1 $
 */

#ifndef REFERENCE_COUNT_REBUILD_H
#define REFERENCE_COUNT_REBUILD_H

#include "types.h"

/**
 * Rebuild the reference counts from the block map (read-only rebuild).
 *
 * @param [in]  vdo                 The VDO
 * @param [in]  parent              The completion to notify when the rebuild is
 *                                  complete
 * @param [out] logicalBlocksUsed   A pointer to hold the logical blocks used
 * @param [out] blockMapDataBlocks  A pointer to hold the number of block map
 *                                  data blocks
 **/
void rebuildReferenceCounts(VDO           *vdo,
                            VDOCompletion *parent,
                            BlockCount    *logicalBlocksUsed,
                            BlockCount    *blockMapDataBlocks);

#endif // REFERENCE_COUNT_REBUILD_H
