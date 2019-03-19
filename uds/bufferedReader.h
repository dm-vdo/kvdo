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
 * $Id: //eng/uds-releases/gloria/src/uds/bufferedReader.h#1 $
 */

#ifndef BUFFERED_READER_H
#define BUFFERED_READER_H 1

#include "common.h"
#include "ioRegion.h"

/**
 * The buffered reader allows efficient IO for IORegions, which may be
 * file- or block-based. The internal buffer always reads aligned data
 * from the underlying region.
 **/
typedef struct bufferedReader BufferedReader;

/**
 * Make a new buffered reader.
 *
 * @param region        An IORegion to read from.
 * @param readerPtr     The pointer to hold the newly allocated buffered
 *                      reader
 *
 * @return UDS_SUCCESS or error code.
 **/
int makeBufferedReader(IORegion *region, BufferedReader **readerPtr)
  __attribute__((warn_unused_result));

/**
 * Free a buffered reader.
 *
 * @param reader        The buffered reader
 **/
void freeBufferedReader(BufferedReader *reader);

/**
 * Retrieve data from a buffered reader, reading from the region when needed.
 *
 * @param reader        The buffered reader
 * @param data          The buffer to read data into
 * @param length        The length of the data to read
 *
 * @return UDS_SUCCESS or an error code.
 **/
int readFromBufferedReader(BufferedReader *reader, void *data, size_t length)
  __attribute__((warn_unused_result));

/**
 * Read data of unknown length from the buffered reader.
 *
 * @param [in]  reader  The buffered reader
 * @param [in]  data    The buffer to read data into
 * @param [in]  size    The size of the data buffer, the maximum to read.
 * @param [out] count   Upon return, the actual number of bytes read.
 *
 * @return UDS_SUCCESS or an error code
 **/
int readBufferedData(BufferedReader *reader,
                     void           *data,
                     size_t          size,
                     size_t         *count)
  __attribute__((warn_unused_result));

/**
 * Verify that the data currently in the buffer matches the required value.
 *
 * @param reader        The buffered reader.
 * @param value         The value that must match the buffer contents.
 * @param length        The length of the value that must match.
 *
 * @return UDS_SUCCESS or an error code, specifically UDS_CORRUPT_FILE
 *         if the required value fails to match.
 *
 * @note If the value matches, the matching contents are consumed. However,
 *       if the match fails, any buffer contents are left as is.
 **/
int verifyBufferedData(BufferedReader *reader,
                       const void     *value,
                       size_t          length)
  __attribute__((warn_unused_result));

/**
 * Get the current position from a buffered reader.
 *
 * @param reader          The buffered reader
 *
 * @return The position of the next unreaded byte in the IORegion.
 **/
off_t getBufferedReaderPosition(BufferedReader *reader);

/**
 * Set the position in the IORregion for the BufferedReader to read from.
 *
 * @param reader        The buffered reader.
 * @param position      The position to read from.
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note Setting the position to any value other than the current one
 *       can cause the current buffer to be discarded, forcing a new
 *       read.
 *
 * @note Setting the position to a value larger than that supported by
 *       the IORegion may not fail until the next read is attempted.
 **/
int setBufferedReaderPosition(BufferedReader *reader, off_t position)
  __attribute__((warn_unused_result));

#endif // BUFFERED_READER_H
