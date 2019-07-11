/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/src/uds/indexLayout.h#3 $
 */

#ifndef INDEX_LAYOUT_H
#define INDEX_LAYOUT_H

#include "accessMode.h"
#include "indexState.h"
#include "ioFactory.h"
#include "ioRegion.h"
#include "uds.h"

typedef struct indexLayout IndexLayout;

/**
 * Construct an index layout.  This is a platform specific function that uses
 * the name string, a flag that indicates old vs. new indices, and a
 * UDSConfiguration (for new indices) to make an IOFactory and invoke
 * makeIndexLayoutFromFactory.
 *
 * @param name       String naming the index.  Each platform will use its own
 *                   conventions to interpret the string, but in general it is
 *                   a space-separated sequence of param=value settings.  For
 *                   backward compatibility a string without an equals is
 *                   treated as a platform-specific default parameter value.
 * @param newLayout  Whether this is a new layout.
 * @param config     The UdsConfiguration required for a new layout.
 * @param layoutPtr  Where to store the new index layout
 *
 * @return UDS_SUCCESS or an error code.
 **/
int makeIndexLayout(const char              *name,
                    bool                     newLayout,
                    const UdsConfiguration   config,
                    IndexLayout            **layoutPtr)
  __attribute__((warn_unused_result));

/**
 * Construct an index layout using an IOFactory.  This method is common to all
 * platforms.
 *
 * @param factory    The IOFactory for the block storage containing the index.
 * @param offset     The offset of the start of the index within the block
 *                   storage address space.
 * @param size       The size in bytes of the space within the block storage
 *                   address space, must be at least as large as that returned
 *                   by udsComputeIndexSize() or an error will result.
 * @param newLayout  Whether this is a new layout.
 * @param config     The UdsConfiguration required for a new layout.
 * @param layoutPtr  Where to store the new index layout
 *
 * @return UDS_SUCCESS or an error code.
 **/
int makeIndexLayoutFromFactory(IOFactory               *factory,
                               off_t                    offset,
                               uint64_t                 size,
                               bool                     newLayout,
                               const UdsConfiguration   config,
                               IndexLayout            **layoutPtr)
  __attribute__((warn_unused_result));

/**
 * Decrement the use count of an index layout.  If the count goes to zero, free
 * the index layout.
 *
 * @param layoutPtr  Where the layout is being stored.  Always reset to NULL.
 **/
void freeIndexLayout(IndexLayout **layoutPtr);

/*****************************************************************************/
int cancelIndexSave(IndexLayout *layout, unsigned int saveSlot)
  __attribute__((warn_unused_result));

/*****************************************************************************/
int commitIndexSave(IndexLayout *layout, unsigned int saveSlot)
  __attribute__((warn_unused_result));

/*****************************************************************************/
int discardIndexSaves(IndexLayout *layout, bool all)
  __attribute__((warn_unused_result));

/**
 * Find the latest index save slot.
 *
 * @param [in]  layout          The single file layout.
 * @param [out] numZonesPtr     Where to store the actual number of zones
 *                                that were saved.
 * @param [out] slotPtr         Where to store the slot number we found.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int findLatestIndexSaveSlot(IndexLayout  *layout,
                            unsigned int *numZonesPtr,
                            unsigned int *slotPtr)
  __attribute__((warn_unused_result));

/**
 * Get another reference to an index layout, incrementing it's use count.
 *
 * @param layout     The index layout.
 * @param layoutPtr  Where the new layout pointer is being stored.
 **/
void getIndexLayout(IndexLayout *layout, IndexLayout **layoutPtr);

/**
 * Open an IORegion for a specified state, mode, kind, and zone.
 *
 * @param layout     The index layout
 * @param slot       The save slot
 * @param mode       One of IO_READ or IO_WRITE.
 * @param kind       The kind if index save region to open.
 * @param zone       The zone number for the region.
 * @param regionPtr  Where to store the region.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int getIndexRegion(IndexLayout   *layout,
                   unsigned int   slot,
                   IOAccessMode   mode,
                   RegionKind     kind,
                   unsigned int   zone,
                   IORegion     **regionPtr)
  __attribute__((warn_unused_result));

/**
 * Obtain the nonce to be used to store or validate the loading of volume index
 * pages.
 *
 * @param [in]  layout   The index layout.
 *
 * @return The nonce to use.
 **/
uint64_t getVolumeNonce(IndexLayout *layout)
  __attribute__((warn_unused_result));

/**
 * Obtain an IORegion for the specified index volume.
 *
 * @param [in]  layout     The index layout.
 * @param [in]  access     The type of access requested.
 * @param [out] regionPtr  Where to put the new region.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int openVolumeRegion(IndexLayout   *layout,
                     IOAccessMode   access,
                     IORegion     **regionPtr)
  __attribute__((warn_unused_result));

/**
 * Read the index configuration.
 *
 * @param layout  the generic index layout
 * @param config  the index configuration to read
 *
 * @return UDS_SUCCESS or an error code
 **/
int readIndexConfig(IndexLayout *layout, UdsConfiguration config)
  __attribute__((warn_unused_result));

/**
 * Determine which index save slot to use for a new index save.
 *
 * Also allocates the masterIndex regions and, if needed, the openChapter
 * region.
 *
 * @param [in]  layout          The index layout.
 * @param [in]  numZones        Actual number of zones currently in use.
 * @param [in]  saveType        The index save type.
 * @param [out] saveSlotPtr     Where to store the save slot number.
 *
 * @return UDS_SUCCESS or an error code
 **/
int setupIndexSaveSlot(IndexLayout   *layout,
                       unsigned int   numZones,
                       IndexSaveType  saveType,
                       unsigned int  *saveSlotPtr)
  __attribute__((warn_unused_result));

/**
 * Write the index configuration.
 *
 * @param layout  the generic index layout
 * @param config  the index configuration to write
 *
 * @return UDS_SUCCESS or an error code
 **/
int writeIndexConfig(IndexLayout *layout, UdsConfiguration config)
  __attribute__((warn_unused_result));

#endif // INDEX_LAYOUT_H
