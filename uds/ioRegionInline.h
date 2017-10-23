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
 * $Id: //eng/uds-releases/flanders/src/uds/ioRegionInline.h#2 $
 */

#ifndef IO_REGION_INLINE_H
#define IO_REGION_INLINE_H

#include "compiler.h"
#include "uds-error.h"

#ifndef IO_REGION_INLINE
# error "Must be included by ioRegion.h"
#endif

typedef struct ioRegionOps {
  int (*close)       (IORegion *);
  int (*getLimit)    (IORegion *, off_t *);
  int (*getDataSize) (IORegion *, off_t *);
  int (*clear)       (IORegion *);
  int (*write)       (IORegion *, off_t, const void *, size_t, size_t);
  int (*read)        (IORegion *, off_t, void *, size_t, size_t *);
  int (*getBlockSize)(IORegion *, size_t *);
  int (*getBestSize) (IORegion *, size_t *);
  int (*syncContents)(IORegion *);
} IORegionOps;

struct ioRegion {
  const IORegionOps *ops;
};

/*****************************************************************************/
static INLINE int closeIORegion(IORegion **regionPtr)
{
  int result = UDS_SUCCESS;
  if ((regionPtr != NULL) && (*regionPtr != NULL)) {
    result = (*regionPtr)->ops->close(*regionPtr);
    if (result == UDS_SUCCESS) {
      *regionPtr = NULL;
    }
  }
  return result;
}

/*****************************************************************************/
static INLINE int getRegionLimit(IORegion *region,
                                 off_t    *limit)
{
  return region->ops->getLimit(region, limit);
}

/*****************************************************************************/
static INLINE int getRegionDataSize(IORegion *region,
                                    off_t    *extent)
{
  return region->ops->getDataSize(region, extent);
}

/*****************************************************************************/
static INLINE int clearRegion(IORegion *region)
{
  return region->ops->clear(region);
}

/*****************************************************************************/
static INLINE int writeToRegion(IORegion   *region,
                                off_t       offset,
                                const void *data,
                                size_t      size,
                                size_t      length)
{
  return region->ops->write(region, offset, data, size, length);
}

/*****************************************************************************/
static INLINE int readFromRegion(IORegion *region,
                                 off_t     offset,
                                 void     *buffer,
                                 size_t    size,
                                 size_t   *length)
{
  return region->ops->read(region, offset, buffer, size, length);
}

/*****************************************************************************/
static INLINE int getRegionBlockSize(IORegion *region,
                                     size_t   *blockSize)
{
  return region->ops->getBlockSize(region, blockSize);
}

/*****************************************************************************/
static INLINE int getRegionBestBufferSize(IORegion *region,
                                          size_t   *bufferSize)
{
  return region->ops->getBestSize(region, bufferSize);
}

/*****************************************************************************/
static INLINE int syncRegionContents(IORegion *region)
{
  return region->ops->syncContents(region);
}

#endif // IO_REGION_INLINE_H
