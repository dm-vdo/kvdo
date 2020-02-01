/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/krusty/src/uds/bufferedWriter.c#1 $
 */

#include "bufferedWriter.h"

#include "compiler.h"
#include "errors.h"
#include "ioFactory.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"


struct bufferedWriter {
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
  // Start of the buffer
  byte                   *bw_start;
  // End of the data written to the buffer
  byte                   *bw_pointer;
  // Error code
  int                     bw_error;
  // Have writes been done?
  bool                    bw_used;
};

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

/*****************************************************************************/
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

/*****************************************************************************/
void freeBufferedWriter(BufferedWriter *bw)
{
  if (bw == NULL) {
    return;
  }
  flushPreviousBuffer(bw);
  int result = -dm_bufio_write_dirty_buffers(bw->bw_client);
  if (result != UDS_SUCCESS) {
    logWarningWithStringError(result, "%s cannot sync storage", __func__);
  }
  dm_bufio_client_destroy(bw->bw_client);
  putIOFactory(bw->bw_factory);
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
    if (bw->bw_buffer == NULL) {
      result = prepareNextBuffer(bw);
      continue;
    }

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
    if (bw->bw_buffer == NULL) {
      result = prepareNextBuffer(bw);
      continue;
    }

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

  return flushPreviousBuffer(bw);
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
