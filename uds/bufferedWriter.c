/*
 * Copyright Red Hat
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
 * $Id: //eng/uds-releases/jasper/src/uds/bufferedWriter.c#6 $
 */

#include "bufferedWriter.h"

#include "compiler.h"
#include "errors.h"
#include "ioFactory.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"


struct bufferedWriter {
#ifdef __KERNEL__
  // IOFactory owning the block device
  IOFactory              *bw_factory;
  // The dm_bufio_client to write to
  struct dm_bufio_client *bw_client;
  // The current dm_buffer
  struct dm_buffer       *bw_buffer;
  // The number of blocks that can be written to
  sector_t                bw_limit;
  // Number of the current block
  sector_t                bw_blockNumber;
#else
  // Region to write to
  IORegion               *bw_region;
  // Number of the current block
  uint64_t                bw_blockNumber;
#endif
  // Start of the buffer
  byte                   *bw_start;
  // End of the data written to the buffer
  byte                   *bw_pointer;
  // Error code
  int                     bw_error;
  // Have writes been done?
  bool                    bw_used;
};

#ifdef __KERNEL__
/*****************************************************************************/
__attribute__((warn_unused_result))
int prepareNextBuffer(BufferedWriter *bw)
{
  if (bw->bw_blockNumber >= bw->bw_limit) {
    bw->bw_error = UDS_OUT_OF_RANGE;
    return UDS_OUT_OF_RANGE;
  }

  struct dm_buffer *buffer = NULL;
  void *data = dm_bufio_new(bw->bw_client, bw->bw_blockNumber, &buffer);
  if (IS_ERR(data)) {
    bw->bw_error = -PTR_ERR(data);
    return bw->bw_error;
  }
  bw->bw_buffer  = buffer;
  bw->bw_start   = data;
  bw->bw_pointer = data;
  return UDS_SUCCESS;
}

/*****************************************************************************/
int flushPreviousBuffer(BufferedWriter *bw)
{
  if (bw->bw_buffer != NULL) {
    if (bw->bw_error == UDS_SUCCESS) {
      size_t avail = spaceRemainingInWriteBuffer(bw);
      if (avail > 0) {
        memset(bw->bw_pointer, 0, avail);
      }
      dm_bufio_mark_buffer_dirty(bw->bw_buffer);
    }
    dm_bufio_release(bw->bw_buffer);
    bw->bw_buffer  = NULL;
    bw->bw_start   = NULL;
    bw->bw_pointer = NULL;
    bw->bw_blockNumber++;
  }
  return bw->bw_error;
}
#endif

/*****************************************************************************/
#ifdef __KERNEL__
int makeBufferedWriter(IOFactory               *factory,
                       struct dm_bufio_client  *client,
                       sector_t                 blockLimit,
                       BufferedWriter         **writerPtr)
{
  BufferedWriter *writer;
  int result = ALLOCATE(1, BufferedWriter, "buffered writer", &writer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  *writer = (BufferedWriter) {
    .bw_factory     = factory,
    .bw_client      = client,
    .bw_buffer      = NULL,
    .bw_limit       = blockLimit,
    .bw_start       = NULL,
    .bw_pointer     = NULL,
    .bw_blockNumber = 0,
    .bw_error       = UDS_SUCCESS,
    .bw_used        = false,
  };

  getIOFactory(factory);
  *writerPtr = writer;
  return UDS_SUCCESS;
}
#else
int makeBufferedWriter(IORegion *region, BufferedWriter **writerPtr)
{
  byte *data;
  int result = ALLOCATE_IO_ALIGNED(UDS_BLOCK_SIZE, byte,
                                   "buffer writer buffer", &data);
  if (result != UDS_SUCCESS) {
    return result;
  }

  BufferedWriter *writer;
  result = ALLOCATE(1, BufferedWriter, "buffered writer", &writer);
  if (result != UDS_SUCCESS) {
    FREE(data);
    return result;
  }

  *writer = (BufferedWriter) {
    .bw_region      = region,
    .bw_start       = data,
    .bw_pointer     = data,
    .bw_blockNumber = 0,
    .bw_error       = UDS_SUCCESS,
    .bw_used        = false,
  };

  getIORegion(region);
  *writerPtr = writer;
  return UDS_SUCCESS;
}
#endif

/*****************************************************************************/
void freeBufferedWriter(BufferedWriter *bw)
{
  if (bw == NULL) {
    return;
  }
#ifdef __KERNEL__
  flushPreviousBuffer(bw);
  int result = -dm_bufio_write_dirty_buffers(bw->bw_client);
#else
  int result = syncRegionContents(bw->bw_region);
#endif
  if (result != UDS_SUCCESS) {
    logWarningWithStringError(result, "%s cannot sync storage", __func__);
  }
#ifdef __KERNEL__
  dm_bufio_client_destroy(bw->bw_client);
  putIOFactory(bw->bw_factory);
#else
  putIORegion(bw->bw_region);
  FREE(bw->bw_start);
#endif
  FREE(bw);
}

/*****************************************************************************/
static INLINE size_t spaceUsedInBuffer(BufferedWriter *bw)
{
  return bw->bw_pointer - bw->bw_start;
}

/*****************************************************************************/
size_t spaceRemainingInWriteBuffer(BufferedWriter *bw)
{
  return UDS_BLOCK_SIZE - spaceUsedInBuffer(bw);
}

/*****************************************************************************/
int writeToBufferedWriter(BufferedWriter *bw, const void *data, size_t len)
{
  if (bw->bw_error != UDS_SUCCESS) {
    return bw->bw_error;
  }

  const byte *dp = data;
  int result = UDS_SUCCESS;
  while ((len > 0) && (result == UDS_SUCCESS)) {
#ifdef __KERNEL__
    if (bw->bw_buffer == NULL) {
      result = prepareNextBuffer(bw);
      continue;
    }
#endif

    size_t avail = spaceRemainingInWriteBuffer(bw);
    size_t chunk = minSizeT(len, avail);
    memcpy(bw->bw_pointer, dp, chunk);
    len            -= chunk;
    dp             += chunk;
    bw->bw_pointer += chunk;

    if (spaceRemainingInWriteBuffer(bw) == 0) {
      result = flushBufferedWriter(bw);
    }
  }

  bw->bw_used = true;
  return result;
}

/*****************************************************************************/
int writeZerosToBufferedWriter(BufferedWriter *bw, size_t len)
{
  if (bw->bw_error != UDS_SUCCESS) {
    return bw->bw_error;
  }

  int result = UDS_SUCCESS;
  while ((len > 0) && (result == UDS_SUCCESS)) {
#ifdef __KERNEL__
    if (bw->bw_buffer == NULL) {
      result = prepareNextBuffer(bw);
      continue;
    }
#endif

    size_t avail = spaceRemainingInWriteBuffer(bw);
    size_t chunk = minSizeT(len, avail);
    memset(bw->bw_pointer, 0, chunk);
    len            -= chunk;
    bw->bw_pointer += chunk;

    if (spaceRemainingInWriteBuffer(bw) == 0) {
      result = flushBufferedWriter(bw);
    }
  }

  bw->bw_used = true;
  return result;
}

/*****************************************************************************/
int flushBufferedWriter(BufferedWriter *bw)
{
  if (bw->bw_error != UDS_SUCCESS) {
    return bw->bw_error;
  }

#ifdef __KERNEL__
  return flushPreviousBuffer(bw);
#else
  size_t n = spaceUsedInBuffer(bw);
  if (n > 0) {
    int result = writeToRegion(bw->bw_region,
                               bw->bw_blockNumber * UDS_BLOCK_SIZE,
                               bw->bw_start, UDS_BLOCK_SIZE, n);
    if (result != UDS_SUCCESS) {
      return bw->bw_error = result;
    } else {
      bw->bw_pointer = bw->bw_start;
      bw->bw_blockNumber++;
    }
  }
  return UDS_SUCCESS;
#endif
}

/*****************************************************************************/
bool wasBufferedWriterUsed(const BufferedWriter *bw)
{
  return bw->bw_used;
}

/*****************************************************************************/
void noteBufferedWriterUsed(BufferedWriter *bw)
{
  bw->bw_used = true;
}
