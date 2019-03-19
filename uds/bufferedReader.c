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
 * $Id: //eng/uds-releases/gloria/src/uds/bufferedReader.c#1 $
 */

#include "bufferedReader.h"
#include "bufferedReaderInternals.h"

#include "compiler.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"

/*****************************************************************************/
int makeBufferedReader(IORegion *region, BufferedReader **readerPtr)
{
  BufferedReader *reader = NULL;
  int result = ALLOCATE(1, BufferedReader, "buffered reader", &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getRegionBestBufferSize(region, &reader->bufsize);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ALLOCATE_IO_ALIGNED(reader->bufsize, byte, "buffered reader buffer",
                               &reader->buffer);
  if (result != UDS_SUCCESS) {
    FREE(reader);
    return result;
  }

  reader->region = region;
  reader->offset = 0;
  reader->extent = reader->bufpos = reader->buffer;
  reader->eof    = false;
  reader->close  = false;
  *readerPtr = reader;
  return UDS_SUCCESS;
}

/*****************************************************************************/
void freeBufferedReader(BufferedReader *reader)
{
  if (reader != NULL) {
    if (reader->close) {
      closeIORegion(&reader->region);
    }
    FREE(reader->buffer);
    FREE(reader);
  }
}

/*****************************************************************************/
int readBufferedData(BufferedReader *reader,
                     void           *data,
                     size_t          length,
                     size_t         *count)
{
  // NOTE implements both itself and readFromBufferedReader() if count == NULL
  // relies on the error handling behavior of readFromRegion when a minimum
  // read length is specified

  int result = UDS_SUCCESS;
  byte *dp = data;

  // first read what's left in the buffer
  size_t remaining = reader->extent - reader->bufpos;
  size_t n = minSizeT(length, remaining);
  if (n > 0) {
    memcpy(dp, reader->bufpos, n);
    reader->bufpos += n;
    dp             += n;
    length         -= n;
  }

  if (reader->eof) {
    result = UDS_END_OF_FILE;
  }

  bool alwaysCopy = false;
  while ((length > 0) && (result == UDS_SUCCESS)) {
    // then read whole bufsize chunks directly, bypassing the buffer
    if ((length >= reader->bufsize) && !alwaysCopy) {
      size_t len = length / reader->bufsize * reader->bufsize;
      n = (count == NULL) ? reader->bufsize : 0;
      result = readFromRegion(reader->region, reader->offset, dp, len, &n);
      if (result == UDS_INCORRECT_ALIGNMENT) {
        // Usually the buffer is not correctly aligned, so switch to
        // copying from the buffer
        result = UDS_SUCCESS;
        alwaysCopy = true;
        continue;
      }
      if (result != UDS_SUCCESS) {
        logWarningWithStringError(result, "%s got readFromRegion error",
                                  __func__);
        break;
      }
      dp             += n;
      length         -= n;
      reader->offset += n;

      if (n < len) {
        reader->eof = true;
        result = UDS_END_OF_FILE;
      }
      continue;
    }

    // then read a buffers' worth and copy some of it
    n = 0;
    result = readFromRegion(reader->region, reader->offset, reader->buffer,
                            reader->bufsize, &n);
    if (result != UDS_SUCCESS) {
      logWarningWithStringError(result, "%s got readFromRegion error",
                                __func__);
      break;
    }

    reader->offset += n;
    reader->bufpos =  reader->buffer;
    reader->extent =  reader->buffer + n;
    if (n < reader->bufsize) {
      reader->eof = true;
      result = UDS_END_OF_FILE;
    }

    n = minSizeT(n, length);
    memcpy(dp, reader->bufpos, n);
    dp             += n;
    length         -= n;
    reader->bufpos += n;
  }

  if ((result == UDS_END_OF_FILE) && (dp - (byte *) data > 0)) {
    result = UDS_SHORT_READ;
  }
  if ((result == UDS_SHORT_READ) && (length == 0)) {
    result = UDS_SUCCESS;
  }
  if (count != NULL) {
    *count = dp - (byte *) data;
    if ((result == UDS_SHORT_READ) || (result == UDS_END_OF_FILE)) {
      // these are not errors when read-like semantics are used
      result = UDS_SUCCESS;
    }
  }
  return result;
}

/*****************************************************************************/
int readFromBufferedReader(BufferedReader *reader, void *data, size_t length)
{
  return readBufferedData(reader, data, length, NULL);
}

/*****************************************************************************/
static INLINE off_t currentPosition(BufferedReader *reader)
{
  return reader->offset - (reader->extent - reader->bufpos);
}

/*****************************************************************************/
static INLINE off_t bufferPosition(BufferedReader *reader)
{
  return reader->offset - (reader->extent - reader->buffer);
}

/*****************************************************************************/
off_t getBufferedReaderPosition(BufferedReader *reader)
{
  return currentPosition(reader);
}

/*****************************************************************************/
static int repositionReader(BufferedReader *reader, off_t position)
{
  if (position > reader->offset) {
    off_t limit = 0;
    int result = getRegionLimit(reader->region, &limit);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (position > limit) {
      return logErrorWithStringError(UDS_OUT_OF_RANGE,
                                     "cannot position beyond region end");
    }
  } else if (position > bufferPosition(reader)) {
    // can just manipulate buffer pointers
    if (position > currentPosition(reader)) {
      reader->bufpos += position - currentPosition(reader);
    } else {
      reader->bufpos -= currentPosition(reader) - position;
    }
    return UDS_SUCCESS;
  } // else before current buffer

  size_t phase = position % reader->bufsize;
  if (phase == 0) {
    reader->bufpos = reader->extent = reader->buffer;
    reader->offset = position;
    reader->eof = false;
  } else {
    size_t n = 0;
    int result = readFromRegion(reader->region, position - phase,
                                reader->buffer, reader->bufsize, &n);
    if (result != UDS_SUCCESS) {
      logWarningWithStringError(result, "%s got readFromRegion error",
                                __func__);
      return result;
    }
    reader->extent = reader->buffer + n;
    reader->bufpos = reader->buffer + phase;
    reader->offset = position + n - phase;
    reader->eof    = (n < reader->bufsize);
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int setBufferedReaderPosition(BufferedReader *reader, off_t position)
{
  return repositionReader(reader, position);
}

/*****************************************************************************/
int verifyBufferedData(BufferedReader *reader,
                       const void     *value,
                       size_t          length)
{
  int          result     = UDS_SUCCESS;
  const byte  *vp         = value;
  const off_t  origOffset = reader->offset;

  // first verify what's left in the buffer
  size_t remaining = reader->extent - reader->bufpos;
  size_t n = minSizeT(length, remaining);
  if (n > 0) {
    if (memcmp(vp, reader->bufpos, n) != 0) {
      return logWarningWithStringError(UDS_CORRUPT_FILE,
                                       "%s got unexpected data", __func__);
    }
    vp             += n;
    reader->bufpos += n;
    length         -= n;
  }

  // unlike readBufferedData, we always read into the buffer
  while (length > 0) {
    if (reader->eof) {
      repositionReader(reader, origOffset);
      return logWarningWithStringError(UDS_CORRUPT_FILE,
                                       "%s got unexpected EOF", __func__);
    }

    n = 0;
    result = readFromRegion(reader->region, reader->offset,
                            reader->buffer, reader->bufsize, &n);
    if (result != UDS_SUCCESS) {
      repositionReader(reader, origOffset);
      return logWarningWithStringError(result, "%s got readFromRegion error",
                                       __func__);
    } else if (n == 0) {
      repositionReader(reader, origOffset);
      return logWarningWithStringError(UDS_CORRUPT_FILE, "%s got no data",
                                       __func__);
    }

    reader->offset += n;
    reader->bufpos =  reader->buffer;
    reader->extent =  reader->buffer + n;

    if (n < reader->bufsize) {
      reader->eof = true;
    }
    n = minSizeT(n, length);
    if (memcmp(vp, reader->bufpos, n) != 0) {
      repositionReader(reader, origOffset);
      return logWarningWithStringError(UDS_CORRUPT_FILE,
                                       "%s got unexpected data", __func__);
    }
    vp             += n;
    length         -= n;
    reader->bufpos += n;
  };

  return UDS_SUCCESS;
}
