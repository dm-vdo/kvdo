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
 * $Id: //eng/uds-releases/gloria/src/uds/blockIORegion.c#1 $
 */

#include "blockIORegion.h"

#include "compiler.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"
#include "stringUtils.h"

typedef struct blockIORegion {
  IORegion       common;
  IORegion      *parent;
  IOAccessMode   access;
  size_t         blockSize;
  size_t         bestSize;
  off_t          start;
  off_t          end;
} BlockIORegion;

/*****************************************************************************/
static INLINE BlockIORegion *asBlockIORegion(IORegion *region)
{
  return container_of(region, BlockIORegion, common);
}

/*****************************************************************************/
static const char *directionOfAccess(IOAccessMode access)
{
  switch (access) {
    case IO_READ:
      return "reading";

    case IO_WRITE:
    case IO_CREATE_WRITE:
      return "writing";

    case IO_READ_WRITE:
    case IO_CREATE_READ_WRITE:
      return "reading, writing";

    default:
      return "[unsupported access type]";
  }
}

/*****************************************************************************/
static int validateIO(const BlockIORegion *bior,
                      off_t                offset,
                      size_t               size,
                      size_t              *length,
                      IOAccessMode         access)
{
  if ((access & bior->access) != access) {
    return logErrorWithStringError(UDS_BAD_IO_DIRECTION,
                                   "not open for %s",
                                   directionOfAccess(access));
  }

  if (offset % bior->blockSize != 0) {
    return logErrorWithStringError(UDS_INCORRECT_ALIGNMENT,
                                   "alignment %zd not multiple of %zd", offset,
                                   bior->blockSize);
  }

  if (size % bior->blockSize != 0) {
    return logErrorWithStringError(UDS_BUFFER_ERROR,
                                   "buffer size %zd not a multiple of %zd",
                                   size, bior->blockSize);
  }

  if (*length > size) {
    return logErrorWithStringError(UDS_BUFFER_ERROR,
                                   "length %zd exceeds buffer size %zd",
                                   *length, size);
  }

  if (offset > bior->end - bior->start) {
    return logErrorWithStringError(UDS_OUT_OF_RANGE,
                                   "offset %zd exceeds limit of %zd",
                                   offset, bior->end - bior->start);
  }

  if ((offset == (off_t) (bior->end - bior->start)) && (*length > 0)) {
    return UDS_END_OF_FILE;
  }

  if ((access & IO_READ) == IO_READ) {
    *length = minSizeT(bior->end - bior->start, offset + size) - offset;
  }
  if ((offset < 0) ||
      ((offset + (ssize_t) *length) > (bior->end - bior->start))) {
    return logErrorWithStringError(UDS_OUT_OF_RANGE,
                                   "range %zd-%zd not in range 0 to %zd",
                                   offset, offset + *length,
                                   bior->end - bior->start);
  }

  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bior_close(IORegion *region)
{
  FREE(asBlockIORegion(region));
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bior_getLimit(IORegion *region,
                         off_t    *limit)
{
  BlockIORegion *bior = asBlockIORegion(region);

  *limit = bior->end - bior->start;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bior_getDataSize(IORegion *region,
                            off_t    *extent)
{
  BlockIORegion *bior = asBlockIORegion(region);

  off_t limit = INT64_MAX;
  int result = getRegionLimit(bior->parent, &limit);
  if (result == UDS_SUCCESS) {
    off_t temp = INT64_MAX;
    result = getRegionDataSize(bior->parent, &temp);
    if (result == UDS_SUCCESS) {
      if (temp < limit) {
        limit = temp;
      }
      if (bior->end < limit) {
        limit = bior->end;
      }
      if (limit < bior->start) {
        *extent = 0;
      } else {
        *extent = limit - bior->start;
      }
    }
  }
  return result;
}

/*****************************************************************************/
static int bior_clear(IORegion *region)
{
  BlockIORegion *bior = asBlockIORegion(region);

  size_t len = 0;

  int result = validateIO(bior, 0, 0, &len, IO_WRITE);
  if (result != UDS_SUCCESS) {
    return result;
  }

  byte *buf;

  result = ALLOCATE_IO_ALIGNED(bior->bestSize, byte, "clearing buffer", &buf);
  if (result != UDS_SUCCESS) {
    return result;
  }
  memset(buf, 0, bior->bestSize);

  off_t offset = bior->start;
  while (offset < bior->end) {
    size_t len = minSizeT(bior->end - offset, bior->bestSize);
    result = writeToRegion(bior->parent, offset, buf, bior->bestSize, len);
    if (result != UDS_SUCCESS) {
      break;
    }
    offset += len;
  }

  FREE(buf);
  return result;
}

/*****************************************************************************/
static int bior_write(IORegion   *region,
                      off_t       offset,
                      const void *data,
                      size_t      size,
                      size_t      len)
{
  BlockIORegion *bior = asBlockIORegion(region);
  int result = validateIO(bior, offset, size, &len, IO_WRITE);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return writeToRegion(bior->parent, bior->start + offset, data, size, len);
}

/*****************************************************************************/
static int bior_read(IORegion *region,
                     off_t     offset,
                     void     *buffer,
                     size_t    size,
                     size_t   *length)
{
  BlockIORegion *bior = asBlockIORegion(region);

  size_t len = (length == NULL) ? size : *length;

  int result = validateIO(bior, offset, size, &len, IO_READ);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = readFromRegion(bior->parent, bior->start + offset,
                          buffer, len, NULL);
  if (result == UDS_SUCCESS && length != NULL) {
    *length = len;
  }
  return result;
}

/*****************************************************************************/
static int bior_getBlockSize(IORegion *region, size_t *blockSize)
{
  *blockSize = asBlockIORegion(region)->blockSize;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bior_getBestSize(IORegion *region, size_t *bufferSize)
{
  *bufferSize = asBlockIORegion(region)->bestSize;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bior_syncContents(IORegion *region)
{
  BlockIORegion *bior = asBlockIORegion(region);

  return syncRegionContents(bior->parent);
}

/*****************************************************************************/
int openBlockRegion(IORegion       *parent,
                    IOAccessMode    access,
                    off_t           start,
                    off_t           end,
                    IORegion      **regionPtr)
{
  if (start > end) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "end (%zd) preceeds start (%zd)", end,
                                   start);
  }

  if (access & ~(IO_ACCESS_MASK)) {
    // in reality we only care about the read and write bits
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "unknown access type %x", access);
  }

  off_t limit = 0;
  int result = getRegionLimit(parent, &limit);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (end > limit) {
    return logErrorWithStringError(UDS_OUT_OF_RANGE, "end (%zd)"
                                   " exceeds underlying device limit (%zd)",
                                   end, limit);
  }

  size_t blockSize = 0;
  size_t bestSize  = 0;
  result = getRegionBlockSize(parent, &blockSize);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getRegionBestBufferSize(parent, &bestSize);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (start % blockSize != 0) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "start offset (%zd) not block aligned",
                                   start);
  }
  if (end % blockSize != 0) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "end offset (%zd) not block aligned", end);
  }

  BlockIORegion *bior;
  result = ALLOCATE(1, BlockIORegion, "open block region", &bior);
  if (result != UDS_SUCCESS) {
    return result;
  }
  bior->common.clear        = bior_clear;
  bior->common.close        = bior_close;
  bior->common.getBestSize  = bior_getBestSize;
  bior->common.getBlockSize = bior_getBlockSize;
  bior->common.getDataSize  = bior_getDataSize;
  bior->common.getLimit     = bior_getLimit;
  bior->common.read         = bior_read;
  bior->common.syncContents = bior_syncContents;
  bior->common.write        = bior_write;
  bior->parent    = parent;
  bior->access    = access;
  bior->blockSize = blockSize;
  bior->bestSize  = bestSize;
  bior->start     = start;
  bior->end       = end;
  *regionPtr = &bior->common;
  return UDS_SUCCESS;
}
