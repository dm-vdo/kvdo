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
 * $Id: //eng/uds-releases/gloria/src/uds/bufferedWriter.h#1 $
 */

#ifndef BUFFERED_WRITER_H
#define BUFFERED_WRITER_H 1

#include "common.h"
#include "ioRegion.h"

typedef struct bufferedWriter BufferedWriter;

/**
 * Make a new buffered writer.
 *
 * @param region        The region to write to.
 * @param bufSize       The size of the buffer, 0 for region's best size.
 *                        Must be a multiple of the region's block size.
 * @param writerPtr     The new buffered writer goes here.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int makeBufferedWriter(IORegion        *region,
                       size_t           bufSize,
                       BufferedWriter **writerPtr)
  __attribute__((warn_unused_result));

/**
 * Free a buffered writer, without flushing.
 *
 * @param [in] buffer   The buffered writer object.
 **/
void freeBufferedWriter(BufferedWriter *buffer);

/**
 * Append data to buffer, writing as needed.
 *
 * @param buffer        The buffered writer object.
 * @param data          The data to write.
 * @param len           The length of the data written.
 *
 * @return              UDS_SUCCESS or an error code.
 *                      The error may reflect previous attempts to write
 *                      or flush the buffer.  Once a write or flush error
 *                      occurs it is sticky.
 **/
int writeToBufferedWriter(BufferedWriter *buffer,
                          const void     *data,
                          size_t          len)
  __attribute__((warn_unused_result));

/**
 * Flush any partial data from the buffer.
 *
 * @param buffer        The buffered writer object.
 *
 * @return              UDS_SUCCESS or an error code.
 *                      The error may reflect previous attempts to write
 *                      or flush the buffer.  Once a write or flush error
 *                      occurs it is sticky.
 **/
int flushBufferedWriter(BufferedWriter *buffer)
  __attribute__((warn_unused_result));

/**
 * Return the size of the remaining space in the buffer (for testing)
 *
 * @param [in] buffer   The buffered writer object.
 *
 * @return              The number of available bytes in the buffer.
 **/
size_t spaceRemainingInWriteBuffer(BufferedWriter *buffer)
  __attribute__((warn_unused_result));

/**
 * Return whether the buffer was ever written to.
 *
 * @param buffer        The buffered writer object.
 *
 * @return              True if at least one call to writeToBufferedWriter
 *                      was made.
 **/
bool wasBufferedWriterUsed(const BufferedWriter *buffer)
  __attribute__((warn_unused_result));

/**
 * Note the buffer has been used.
 *
 * @param buffer        The buffered writer object.
 **/
void noteBufferedWriterUsed(BufferedWriter *buffer);

#endif // BUFFERED_WRITER_H
