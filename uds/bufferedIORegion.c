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
 * $Id: //eng/uds-releases/gloria/src/uds/bufferedIORegion.c#1 $
 */

#include "bufferedIORegion.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

typedef struct bufferedIORegion {
  IORegion  common;
  Buffer   *buffer;
  bool      myBuffer;
  size_t    bufSize;
  off_t     position;
} BufferedIORegion;

/*****************************************************************************/
static INLINE BufferedIORegion *asBufferedIORegion(IORegion *region)
{
  return container_of(region, BufferedIORegion, common);
}

/*****************************************************************************/
int getBufferedRegionBuffer(IORegion  *region,
                            bool       release,
                            Buffer   **bufferPtr)
{
  BufferedIORegion *bufior = asBufferedIORegion(region);

  if (release) {
    bufior->myBuffer = false;
  }
  *bufferPtr = bufior->buffer;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bufior_close(IORegion *region)
{
  BufferedIORegion *bufior = asBufferedIORegion(region);

  if (bufior->myBuffer) {
    freeBuffer(&bufior->buffer);
  }
  FREE(bufior);
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bufior_getLimit(IORegion *region __attribute__((unused)),
                           off_t    *limit)
{
  *limit = INT64_MAX;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bufior_getDataSize(IORegion *region,
                              off_t    *extent)
{
  BufferedIORegion *bufior = asBufferedIORegion(region);

  *extent = bufior->position + bufferUsed(bufior->buffer);
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bufior_clear(IORegion *region)
{
  BufferedIORegion *bufior = asBufferedIORegion(region);

  return resetBufferEnd(bufior->buffer, 0);
}

/*****************************************************************************/
static int bufior_write(IORegion   *region,
                        off_t       offset,
                        const void *data,
                        size_t      size __attribute__((unused)),
                        size_t      length)
{
  BufferedIORegion *bufior = asBufferedIORegion(region);
  Buffer *buf = bufior->buffer;

  int  result   = UDS_SUCCESS;
  off_t currentOffset = bufior->position + bufferUsed(buf);

  if (offset < currentOffset) {
    if (offset < bufior->position) {
      return UDS_BUFFER_ERROR;
    }
    result = resetBufferEnd(buf, offset - bufior->position);
    if (result != UDS_SUCCESS) {
      return result;
    }
  } else {
    size_t padding = offset - currentOffset;
    size_t needed = padding + length;

    if (needed > availableSpace(buf)) {
      size_t old = uncompactedAmount(buf);
      size_t len = needed - availableSpace(buf) + bufferLength(buf);
      size_t n = (len + bufferLength(buf) - 1) / bufferLength(buf);
      result = growBuffer(buf, n * bufferLength(buf));
      if (result != UDS_SUCCESS) {
        return result;
      }
      bufior->position += old - uncompactedAmount(buf);
    }

    if (padding > 0) {
      result = zeroBytes(buf, padding);
      if (result != UDS_SUCCESS) {
        return result;
      }
    }
  }

  if (length > 0) {
    result = putBytes(buf, length, data);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  if ((offset < currentOffset) && (currentOffset > offset + (off_t) length)) {
    result = resetBufferEnd(buf, currentOffset - bufior->position);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bufior_read(IORegion *region,
                       off_t     offset,
                       void     *buffer,
                       size_t    size,
                       size_t   *length)
{
  BufferedIORegion *bufior = asBufferedIORegion(region);

  int   result  = UDS_SUCCESS;
  off_t currentOffset = bufior->position + uncompactedAmount(bufior->buffer);

  if (offset < currentOffset) {
    int result = rewindBuffer(bufior->buffer, currentOffset - offset);
    if (result != UDS_SUCCESS) {
      return result;
    }
    currentOffset = offset;
  }

  if (offset > currentOffset) {
    result = skipForward(bufior->buffer, offset - currentOffset);
    if (result != UDS_SUCCESS) {
      return result;
    }
    currentOffset = offset;
  }

  size_t len = (length == NULL) ? size : *length;
  size_t n = minSizeT(size, contentLength(bufior->buffer));

  if (n > 0) {
    int result = getBytesFromBuffer(bufior->buffer, n, buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  if (n < len) {
    if (n == 0) {
      return logErrorWithStringError(UDS_END_OF_FILE,
                                     "expected at least %zd bytes, got EOF",
                                     len);
    } else {
      return logErrorWithStringError(UDS_SHORT_READ,
                                     "expected at least %zd bytes, got %zd",
                                     len, n);
    }
  }
  if (length != NULL) {
    *length = n;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bufior_getBlockSize(IORegion *region __attribute__((unused)),
                               size_t   *blockSize)
{
  *blockSize = 1;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bufior_getBestSize(IORegion *region,
                              size_t   *bufferSize)
{
  *bufferSize = asBufferedIORegion(region)->bufSize;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int bufior_syncContents(IORegion *region __attribute__((unused)))
{
  return UDS_SUCCESS;
}

/*****************************************************************************/
int makeBufferedRegion(Buffer    *buffer,
                       size_t     bufferSize,
                       IORegion **regionPtr)
{
  BufferedIORegion *bufior = NULL;
  int result = ALLOCATE(1, BufferedIORegion, "buffered IO region", &bufior);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (buffer == NULL) {
    if (bufferSize == 0) {
      bufferSize = 1024;
    }
    result = makeBuffer(bufferSize, &buffer);
    bufior->myBuffer = true;
  } else {
    if (availableSpace(buffer) < bufferSize) {
      result = growBuffer(buffer, bufferSize);
    }
  }
  if (result != UDS_SUCCESS) {
    FREE(bufior);
    return result;
  }

  bufior->common.clear        = bufior_clear;
  bufior->common.close        = bufior_close;
  bufior->common.getBestSize  = bufior_getBestSize;
  bufior->common.getBlockSize = bufior_getBlockSize;
  bufior->common.getDataSize  = bufior_getDataSize;
  bufior->common.getLimit     = bufior_getLimit;
  bufior->common.read         = bufior_read;
  bufior->common.syncContents = bufior_syncContents;
  bufior->common.write        = bufior_write;
  bufior->buffer   = buffer;
  bufior->bufSize  = availableSpace(buffer);
  bufior->position = 0;
  *regionPtr = &bufior->common;
  return UDS_SUCCESS;
}
