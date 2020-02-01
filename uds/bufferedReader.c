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
 * $Id: //eng/uds-releases/krusty/src/uds/bufferedReader.c#1 $
 */

#include "bufferedReader.h"

#include "compiler.h"
#include "ioFactory.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"


struct bufferedReader {
  // IOFactory owning the block device
  IOFactory              *br_factory;
  // The dm_bufio_client to read from
  struct dm_bufio_client *br_client;
  // The current dm_buffer
  struct dm_buffer       *br_buffer;
  // The number of blocks that can be read from
  sector_t                br_limit;
  // Number of the current block
  sector_t                br_blockNumber;
  // Start of the buffer
  byte                   *br_start;
  // End of the data read from the buffer
  byte                   *br_pointer;
};

/*****************************************************************************/
static void readAhead(BufferedReader *br, sector_t blockNumber)
{
  if (blockNumber < br->br_limit) {
    enum { MAX_READ_AHEAD = 4 };
    size_t readAhead = minSizeT(MAX_READ_AHEAD, br->br_limit - blockNumber);
    dm_bufio_prefetch(br->br_client, blockNumber, readAhead);
  }
}

/*****************************************************************************/
int makeBufferedReader(IOFactory               *factory,
                       struct dm_bufio_client  *client,
                       sector_t                 blockLimit,
                       BufferedReader         **readerPtr)
{
  BufferedReader *reader = NULL;
  int result = ALLOCATE(1, BufferedReader, "buffered reader", &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  *reader = (BufferedReader) {
    .br_factory     = factory,
    .br_client      = client,
    .br_buffer      = NULL,
    .br_limit       = blockLimit,
    .br_blockNumber = 0,
    .br_start       = NULL,
    .br_pointer     = NULL,
  };
  
  readAhead(reader,0);
  getIOFactory(factory);
  *readerPtr = reader;
  return UDS_SUCCESS;
}

/*****************************************************************************/
void freeBufferedReader(BufferedReader *br)
{
  if (br == NULL) {
    return;
  }
  if (br->br_buffer != NULL) {
    dm_bufio_release(br->br_buffer);
  }
  dm_bufio_client_destroy(br->br_client);
  putIOFactory(br->br_factory);
  FREE(br);
}

/*****************************************************************************/
static int positionReader(BufferedReader *br,
                          sector_t        blockNumber,
                          off_t           offset)
{
  if ((br->br_pointer == NULL) || (blockNumber != br->br_blockNumber)) {
    if (blockNumber >= br->br_limit) {
      return UDS_OUT_OF_RANGE;
    }
    if (br->br_buffer != NULL) {
      dm_bufio_release(br->br_buffer);
      br->br_buffer = NULL;
    }
    struct dm_buffer *buffer = NULL;
    void *data = dm_bufio_read(br->br_client, blockNumber, &buffer);
    if (IS_ERR(data)) {
      return -PTR_ERR(data);
    }
    br->br_buffer = buffer;
    br->br_start  = data;
    if (blockNumber == br->br_blockNumber + 1) {
      readAhead(br, blockNumber + 1);
    }
  }
  br->br_blockNumber = blockNumber;
  br->br_pointer     = br->br_start + offset;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static size_t bytesRemainingInReadBuffer(BufferedReader *br)
{
  return (br->br_pointer == NULL
          ? 0
          : br->br_start + UDS_BLOCK_SIZE - br->br_pointer);
}

/*****************************************************************************/
int readFromBufferedReader(BufferedReader *br, void *data, size_t length)
{
  byte *dp = data;
  int result = UDS_SUCCESS;
  while (length > 0) {
    if (bytesRemainingInReadBuffer(br) == 0) {
      sector_t blockNumber = br->br_blockNumber;
      if (br->br_pointer != NULL) {
        ++blockNumber;
      }
      result = positionReader(br, blockNumber, 0);
      if (result != UDS_SUCCESS) {
        break;
      }
    }

    size_t avail = bytesRemainingInReadBuffer(br);
    size_t chunk = minSizeT(length, avail);
    memcpy(dp, br->br_pointer, chunk);
    length         -= chunk;
    dp             += chunk;
    br->br_pointer += chunk;
  }

  if (((result == UDS_OUT_OF_RANGE) || (result == UDS_END_OF_FILE))
      && (dp - (byte *) data > 0)) {
    result = UDS_SHORT_READ;
  }
  return result;
}

/*****************************************************************************/
int verifyBufferedData(BufferedReader *br,
                       const void     *value,
                       size_t          length)
{
  const byte *vp = value;
  sector_t startingBlockNumber = br->br_blockNumber;
  int      startingOffset      = br->br_pointer - br->br_start;
  while (length > 0) {
    if (bytesRemainingInReadBuffer(br) == 0) {
      sector_t blockNumber = br->br_blockNumber;
      if (br->br_pointer != NULL) {
        ++blockNumber;
      }
      int result = positionReader(br, blockNumber, 0);
      if (result != UDS_SUCCESS) {
        positionReader(br, startingBlockNumber, startingOffset);
        return UDS_CORRUPT_FILE;
      }
    }

    size_t avail = bytesRemainingInReadBuffer(br);
    size_t chunk = minSizeT(length, avail);
    if (memcmp(vp, br->br_pointer, chunk) != 0) {
      positionReader(br, startingBlockNumber, startingOffset);
      return UDS_CORRUPT_FILE;
    }
    length         -= chunk;
    vp             += chunk;
    br->br_pointer += chunk;
  }

  return UDS_SUCCESS;
}
