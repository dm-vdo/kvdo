/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/src/uds/indexLayoutInline.h#2 $
 */

#ifndef INDEX_LAYOUT_INLINE_H
#define INDEX_LAYOUT_INLINE_H

#ifndef INDEX_LAYOUT_INLINE
# error "Must be included by indexLayout.h"
#endif

#include "compiler.h"

typedef struct indexLayoutOps {
  void        (*freeFunc)        (IndexLayout *);
  int         (*writeSeal)       (IndexLayout *);
  int         (*removeSeal)      (IndexLayout *);
  int         (*checkIndexExists)(IndexLayout *, bool *);
  int         (*checkSealed)     (IndexLayout *, bool *);
  int         (*writeConfig)     (IndexLayout *, UdsConfiguration);
  int         (*readConfig)      (IndexLayout *layout, UdsConfiguration);
  int         (*openVolumeRegion)(IndexLayout *, unsigned int,
                                  IOAccessMode, IORegion **);
  int         (*makeIndexState)  (IndexLayout *, unsigned int, unsigned int,
                                  unsigned int, IndexState **);
  int         (*getVolumeNonce)  (IndexLayout *, unsigned int, uint64_t *);
  const char *(*name)            (IndexLayout *);
} IndexLayoutOps;

struct indexLayout {
  const IndexLayoutOps *ops;
};

/*****************************************************************************/
static INLINE void freeIndexLayout(IndexLayout **layoutPtr)
{
  if (*layoutPtr != NULL) {
    (*layoutPtr)->ops->freeFunc(*layoutPtr);
    *layoutPtr = NULL;
  }
}

/*****************************************************************************/
static INLINE int writeSafetySeal(IndexLayout *layout)
{
  return layout->ops->writeSeal(layout);
}

/*****************************************************************************/
static INLINE int removeSafetySeal(IndexLayout *layout)
{
  return layout->ops->removeSeal(layout);
}

/*****************************************************************************/
static INLINE int checkIndexExists(IndexLayout *layout, bool *exists)
{
  return layout->ops->checkIndexExists(layout, exists);
}

/*****************************************************************************/
static INLINE int checkIndexIsSealed(IndexLayout *layout, bool *sealed)
{
  return layout->ops->checkSealed(layout, sealed);
}

/*****************************************************************************/
static INLINE int writeIndexConfig(IndexLayout      *layout,
                                   UdsConfiguration  config)
{
  return layout->ops->writeConfig(layout, config);
}

/*****************************************************************************/
static INLINE int readIndexConfig(IndexLayout      *layout,
                                  UdsConfiguration  config)
{
  return layout->ops->readConfig(layout, config);
}

/*****************************************************************************/
static INLINE int openVolumeRegion(IndexLayout   *layout,
                                   unsigned int   indexId,
                                   IOAccessMode   access,
                                   IORegion     **regionPtr)
{
  return layout->ops->openVolumeRegion(layout, indexId, access, regionPtr);
}

/*****************************************************************************/
static INLINE int makeIndexState(IndexLayout   *layout,
                                 unsigned int   indexId,
                                 unsigned int   numZones,
                                 unsigned int   components,
                                 IndexState   **statePtr)
{
  return layout->ops->makeIndexState(layout, indexId, numZones, components,
                                     statePtr);
}

/*****************************************************************************/
static INLINE int getVolumeNonce(IndexLayout  *layout,
                                 unsigned int  indexId,
                                 uint64_t     *nonce)
{
  return layout->ops->getVolumeNonce(layout, indexId, nonce);
}

/*****************************************************************************/
static INLINE const char *indexLayoutName(IndexLayout *layout)
{
  return layout->ops->name(layout);
}

#endif // INDEX_LAYOUT_INLINE_H
