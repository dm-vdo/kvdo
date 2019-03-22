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
 * $Id: //eng/uds-releases/jasper/src/uds/bufferOps.h#1 $
 */

#ifndef BUFFER_OPS_H
#define BUFFER_OPS_H

#include "buffer.h"

/**
 * Read into a buffer from a descriptor, but give up if the read is
 * interrupted. Data will be read into the buffer starting at the
 * current end, up to the buffer's current length.
 *
 * @param buffer    The buffer to read into
 * @param fd        The descriptor to read
 *
 * @return UDS_SUCCESS or an error code
 **/
int readIntoBufferInterruptible(Buffer *buffer, int fd)
  __attribute__((warn_unused_result));

/**
 * Read into a buffer from a descriptor. Data will be read into the buffer
 * starting at the current end, up to the buffer's current length.
 *
 * @param buffer    The buffer to read into
 * @param fd        The descriptor to read
 *
 * @return UDS_SUCCESS or an error code
 **/
int readIntoBuffer(Buffer *buffer, int fd) __attribute__((warn_unused_result));

/**
 * Write from a buffer to a descriptor, but give up if the write is
 * interrupted. Data will be written from the buffer starting at the
 * current start, up to the buffer's current end.
 *
 * @param buffer The buffer to write from
 * @param sock   The socket to write to
 *
 * @return UDS_SUCCESS or an error code
 **/
int sendFromBufferInterruptible(Buffer *buffer, int sock)
  __attribute__((warn_unused_result));

/**
 * Send from a buffer to a socket. Data will be written from the buffer
 * starting at the current start, up to the buffer's current end.
 *
 * @param buffer       The buffer to write from
 * @param sock         The socket to write to
 *
 * @return UDS_SUCCESS or an error code
 **/
int sendFromBuffer(Buffer *buffer, int sock)
  __attribute__((warn_unused_result));

/**
 * Send the complete contents of a buffer via a socket. The buffer will be
 * compacted.
 *
 * @param buffer The buffer to write from
 * @param sock   The socket to write to
 *
 * @return UDS_SUCCESS or an error code
 **/
int sendBufferContents(Buffer *buffer, int sock)
  __attribute__((warn_unused_result));

#endif // BUFFER_OPS_H
