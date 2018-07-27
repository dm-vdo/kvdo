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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vioPool.c#2 $
 */

#include "vioPool.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "constants.h"
#include "vio.h"
#include "types.h"

typedef struct {
  size_t        poolSize;
  char         *buffer;
  VIOPoolEntry  entries[];
} VIOPoolEntries;

/**********************************************************************/
int makeVIOPool(PhysicalLayer   *layer,
                size_t           poolSize,
                VIOConstructor  *vioConstructor,
                void            *context,
                ObjectPool     **poolPtr)
{
  VIOPoolEntries *vioPoolEntries;
  int result = ALLOCATE_EXTENDED(VIOPoolEntries, poolSize, VIOPoolEntry,
                                 __func__, &vioPoolEntries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(poolSize * VDO_BLOCK_SIZE, char, "VIO pool buffer",
                    &vioPoolEntries->buffer);
  if (result != VDO_SUCCESS) {
    FREE(vioPoolEntries);
    return result;
  }

  ObjectPool *pool;
  result = makeObjectPool(vioPoolEntries, &pool);
  if (result != VDO_SUCCESS) {
    FREE(vioPoolEntries->buffer);
    FREE(vioPoolEntries);
    return result;
  }

  char *ptr = vioPoolEntries->buffer;
  for (size_t i = 0; i < poolSize; i++) {
    VIOPoolEntry *entry = &vioPoolEntries->entries[i];
    entry->buffer       = ptr;
    entry->context      = context;
    result = vioConstructor(layer, entry, ptr, &entry->vio);
    if (result != VDO_SUCCESS) {
      freeVIOPool(&pool);
      return result;
    }
    ptr += VDO_BLOCK_SIZE;
    addEntryToObjectPool(pool, &entry->node);
  }

  vioPoolEntries->poolSize = poolSize;

  *poolPtr = pool;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeVIOPool(ObjectPool **poolPtr)
{
  if (*poolPtr == NULL) {
    return;
  }

  // Remove all available entries from the object pool.
  ObjectPool *pool = *poolPtr;
  VIOPoolEntry *entry;
  while ((entry = asVIOPoolEntry(removeEntryFromObjectPool(pool))) != NULL) {
    freeVIO(&entry->vio);
  }

  VIOPoolEntries *vioPoolEntries = getObjectPoolEntryData(pool);
  // Make sure every VIOPoolEntry has been removed.
  for (size_t i = 0; i < vioPoolEntries->poolSize; i++) {
    VIOPoolEntry *entry = &vioPoolEntries->entries[i];
    ASSERT_LOG_ONLY(isRingEmpty(&entry->node), "VIO Pool entry still in use:"
                    " VIO is in use for physical block %" PRIu64
                    " for operation %u",
                    entry->vio->physical,
                    entry->vio->operation);
  }

  freeObjectPool(&pool);
  FREE(vioPoolEntries->buffer);
  FREE(vioPoolEntries);
  *poolPtr = NULL;
}

/**********************************************************************/
void returnVIOToPool(ObjectPool *pool, VIOPoolEntry *entry)
{
  entry->vio->completion.errorHandler = NULL;
  returnEntryToObjectPool(pool, &entry->node);
}
