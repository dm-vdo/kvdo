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
 * $Id: //eng/uds-releases/gloria/src/uds/indexLayout.h#3 $
 */

#ifndef INDEX_LAYOUT_H
#define INDEX_LAYOUT_H

#include "accessMode.h"
#include "compiler.h"
#include "indexState.h"
#include "ioRegion.h"
#include "uds.h"

/**
 * An IndexLayout is an abstract type which can either represent a multi-file
 * layout or a single-file layout, depending on which creation function is
 * used.  Certain generic operations are provided on both types.  These are all
 * called via the inline wrapper methods defined below.
 **/
typedef struct indexLayout {
  int      (*checkIndexExists)(struct indexLayout *, bool *);
  int      (*checkSealed)     (struct indexLayout *, bool *);
  void     (*free)            (struct indexLayout *);
  uint64_t (*getVolumeNonce)  (struct indexLayout *);
  int      (*makeIndexState)  (struct indexLayout *, unsigned int,
                               unsigned int, IndexState **);
  int      (*openVolumeRegion)(struct indexLayout *, IOAccessMode,
                               IORegion **);
  int      (*readConfig)      (struct indexLayout *, UdsConfiguration);
  int      (*removeSeal)      (struct indexLayout *);
  int      (*writeConfig)     (struct indexLayout *, UdsConfiguration);
  int      (*writeSeal)       (struct indexLayout *);
} IndexLayout;

/**
 * Construct an index layout.  This is a platform specific function that maps
 * the info string to type of IndexLayout and invokes the proper constructor.
 *
 * @param name       String nominating the index. Each platform will use its
 *                   own conventions to interpret the string, but in general it
 *                   is a space-separated sequence of param=value settings. For
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
 * Check if the index already exists.
 *
 * @param [in]  layout  the generic index layout
 * @param [out] exists  set to whether the index exists
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static INLINE int checkIndexExists(IndexLayout *layout, bool *exists)
{
  return layout->checkIndexExists(layout, exists);
}

/**
 * Check if the index is sealed.
 *
 * @param [in]  layout  the generic index layout
 * @param [out] sealed  set to whether the index is sealed
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static INLINE int checkIndexIsSealed(IndexLayout *layout, bool *sealed)
{
  return layout->checkSealed(layout, sealed);
}

/**
 * Free an index layout.
 *
 * @param layoutPtr  Where the generic layout is being stored. Set to NULL.
 **/
static INLINE void freeIndexLayout(IndexLayout **layoutPtr)
{
  if (*layoutPtr != NULL) {
    (*layoutPtr)->free(*layoutPtr);
    *layoutPtr = NULL;
  }
}

/**
 * Obtain the nonce to be used to store or validate the loading of volume index
 * pages.
 *
 * @param [in]  layout   The index layout.
 *
 * @return The nonce to use.
 **/
__attribute__((warn_unused_result))
static INLINE uint64_t getVolumeNonce(IndexLayout *layout)
{
  return layout->getVolumeNonce(layout);
}

/**
 * Make an index state object compatible with this layout.
 *
 * @param [in]  layout         The index layout.
 * @param [in]  numZones       The number of zones to use.
 * @param [in]  components     The maximum number of components to be handled.
 * @param [out] statePtr       Where to store the index state object.
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static INLINE int makeIndexState(IndexLayout   *layout,
                                 unsigned int   numZones,
                                 unsigned int   components,
                                 IndexState   **statePtr)
{
  return layout->makeIndexState(layout, numZones, components, statePtr);
}

/**
 * Obtain an IORegion for the specified index volume.
 *
 * @param [in]  layout     The index layout.
 * @param [in]  access     The type of access requested.
 * @param [out] regionPtr  Where to put the new region.
 *
 * @return UDS_SUCCESS or an error code.
 **/
__attribute__((warn_unused_result))
static INLINE int openVolumeRegion(IndexLayout   *layout,
                                   IOAccessMode   access,
                                   IORegion     **regionPtr)
{
  return layout->openVolumeRegion(layout, access, regionPtr);
}

/**
 * Read the index configuration.
 *
 * @param layout  the generic index layout
 * @param config  the index configuration to read
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static INLINE int readIndexConfig(IndexLayout *layout, UdsConfiguration config)
{
  return layout->readConfig(layout, config);
}

/**
 * Unconditionally remove the safety seal of the index layout.
 *
 * @param layout  the generic index layout
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static INLINE int removeSafetySeal(IndexLayout *layout)
{
  return layout->removeSeal(layout);
}

/**
 * Write the index configuration.
 *
 * @param layout        the generic index layout
 * @param config        the index configuration to write
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static INLINE int writeIndexConfig(IndexLayout      *layout,
                                   UdsConfiguration  config)
{
  return layout->writeConfig(layout, config);
}

/**
 * Write the safety seal for the index layout.
 *
 * @param layout  the generic index layout
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static INLINE int writeSafetySeal(IndexLayout *layout)
{
  return layout->writeSeal(layout);
}

#endif // INDEX_LAYOUT_H
