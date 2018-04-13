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
 * $Id: //eng/uds-releases/gloria/src/uds/bufferedWriterInternals.h#1 $
 */

#ifndef BUFFERED_WRITER_INTERNALS_H
#define BUFFERED_WRITER_INTERNALS_H

#include "bufferedWriter.h"

struct bufferedWriter {
  IORegion *bw_region;             ///< region to write to
  off_t     bw_pos;                ///< offset of start of buffer
  size_t    bw_size;               ///< size of buffer
  char     *bw_buf;                ///< start of buffer
  char     *bw_ptr;                ///< end of written data
  int       bw_err;                ///< error code
  bool      bw_used;               ///< have writes been done?
  bool      bw_close;              ///< do we have to close the region?
};

#endif // BUFFERED_WRITER_INTERNALS_H
