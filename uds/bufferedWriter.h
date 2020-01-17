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
 * $Id: //eng/uds-releases/jasper/src/uds/bufferedWriter.h#5 $
 */

#ifndef BUFFERED_WRITER_H
#define BUFFERED_WRITER_H 1

#include "common.h"

struct dm_bufio_client;
struct ioFactory;

typedef struct bufferedWriter BufferedWriter;

/**
 * Make a new buffered writer.
 *
 * @param factory       The IOFactory creating the buffered writer
 * @param client        The dm_bufio_client to write to.
 * @param blockLimit    The number of blocks that may be written to.
 * @param writerPtr     The new buffered writer goes here.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int makeBufferedWriter(struct ioFactory        *factory,
                       struct dm_bufio_client  *client,
                       sector_t                 blockLimit,
                       BufferedWriter         **writerPtr)
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
int writeToBufferedWriter(BufferedWriter *buffer, const void *data, size_t len)
  __attribute__((warn_unused_result));

/**
 * Zero data in the buffer, writing as needed.
 *
 * @param buffer        The buffered writer object.
 * @param len           The number of zero bytes to write.
 *
 * @return              UDS_SUCCESS or an error code.
 *                      The error may reflect previous attempts to write
 *                      or flush the buffer.  Once a write or flush error
 *                      occurs it is sticky.
 **/
int writeZerosToBufferedWriter(BufferedWriter *bw, size_t len)
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
