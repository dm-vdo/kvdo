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
 * $Id: //eng/uds-releases/gloria/src/uds/bufferedReaderInternals.h#1 $
 */

#ifndef BUFFERED_READER_INTERNALS_H
#define BUFFERED_READER_INTERNALS_H

#include "bufferedReader.h"

struct bufferedReader {
  IORegion *region;     // the region to read from
  size_t    bufsize;    // size of buffer
  off_t     offset;     // offset of last byte in the buffer
  byte     *buffer;     // buffer
  byte     *extent;     // extent of the data in the buffer
  byte     *bufpos;     // the next unread byte in the buffer
  bool      eof;        // short read or eof encountered
  bool      close;      // close the IO region when done
};

// Invariants:
//   eof || offset % bufsize == 0
//   buffer <= bufpos <= extent
//   extent - buffer <= bufsize

#endif // BUFFERED_READER_INTERNALS_H
