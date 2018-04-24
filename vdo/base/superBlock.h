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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/superBlock.h#2 $
 */

#ifndef SUPER_BLOCK_H
#define SUPER_BLOCK_H

#include "buffer.h"

#include "completion.h"
#include "types.h"

typedef struct superBlock SuperBlock;

/**
 * Make a new super block.
 *
 * @param [in]  layer          The layer on which to write this super block
 * @param [out] superBlockPtr  A pointer to hold the new super block
 *
 * @return VDO_SUCCESS or an error
 **/
int makeSuperBlock(PhysicalLayer *layer, SuperBlock **superBlockPtr)
  __attribute__((warn_unused_result));

/**
 * Free a super block and null out the reference to it.
 *
 * @param superBlockPtr the reference to the super block to free
 **/
void freeSuperBlock(SuperBlock **superBlockPtr);

/**
 * Save a super block.
 *
 * @param layer             The physical layer on which to save the super block
 * @param superBlock        The super block to save
 * @param superBlockOffset  The location of the super block
 *
 * @return VDO_SUCCESS or an error
 **/
int saveSuperBlock(PhysicalLayer       *layer,
                   SuperBlock          *superBlock,
                   PhysicalBlockNumber  superBlockOffset)
  __attribute__((warn_unused_result));

/**
 * Save a super block asynchronously.
 *
 * @param superBlock        The super block to save
 * @param superBlockOffset  The location at which to write the super block
 * @param parent            The object to notify when the save is complete
 **/
void saveSuperBlockAsync(SuperBlock          *superBlock,
                         PhysicalBlockNumber  superBlockOffset,
                         VDOCompletion       *parent);

/**
 * Allocate a super block and read its contents from storage.
 *
 * @param [in]  layer             The layer from which to load the super block
 * @param [in]  superBlockOffset  The location from which to read the super
 *                                block
 * @param [out] superBlockPtr     A pointer to hold the loaded super block
 *
 * @return VDO_SUCCESS or an error
 **/
int loadSuperBlock(PhysicalLayer        *layer,
                   PhysicalBlockNumber   superBlockOffset,
                   SuperBlock          **superBlockPtr)
  __attribute__((warn_unused_result));

/**
 * Allocate a super block and read its contents from storage asynchronously. If
 * a load error occurs before the super block's own completion can be allocated,
 * the parent will be finished with the error.
 *
 * @param [in]  parent            The completion to finish after loading the
 *                                super block
 * @param [in]  superBlockOffset  The location from which to read the super
 *                                block
 * @param [out] superBlockPtr     A pointer to hold the super block
 **/
void loadSuperBlockAsync(VDOCompletion        *parent,
                         PhysicalBlockNumber   superBlockOffset,
                         SuperBlock          **superBlockPtr);

/**
 * Get a buffer which contains the component data from a super block.
 *
 * @param superBlock  The super block from which to get the component data
 *
 * @return the component data in a buffer
 **/
Buffer *getComponentBuffer(SuperBlock *superBlock)
  __attribute__((warn_unused_result));

/**
 * Get the release version number that was loaded from the volume when the
 * SuperBlock was decoded.
 *
 * @param superBlock  The super block to query
 *
 * @return the release version number that was decoded from the volume
 **/
ReleaseVersionNumber getLoadedReleaseVersion(const SuperBlock *superBlock)
  __attribute__((warn_unused_result));

/**
 * Get the encoded size of the fixed (non-component data) portion of a super
 * block (this is for unit testing).
 *
 * @return The encoded size of the fixed portion of the super block
 **/
size_t getFixedSuperBlockSize(void)
  __attribute__((warn_unused_result));

#endif /* SUPER_BLOCK_H */
