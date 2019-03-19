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
 * $Id: //eng/uds-releases/gloria/src/uds/bufferedIORegion.h#1 $
 */

#ifndef BUFFERED_IO_REGION_H
#define BUFFERED_IO_REGION_H

#include "ioRegion.h"

#include "buffer.h"

/**
 * Create a buffered IO region, intended to hold small amounts of data
 * using the IORegion interface.
 *
 * @param buffer        Either an existing buffer or NULL, in which case a
 *                        new buffer is made.
 * @param bufferSize    The minimum size for the buffer. If specified as 0,
 *                        a default size is chosen for new buffers, and
 *                        existing buffers are not altered. Otherwise the
 *                        existing buffer may be enlarged to the minimum size
 *                        if necessary.
 * @param regionPtr     Where to put the region.
 *
 * @return UDS_SUCCESS or an error code.
 *
 * @note Unlike most IORegions, these are designed with no alignment
 *       constraints. Writing beyond the size of the buffer will cause
 *       the buffer to grow, which can be inefficient.
 *
 * @note If the region creates a buffer it will ordinarily delete it when
 *       it is closed, unless getBufferedRegionBuffer() is called with the
 *       release flag set.
 **/
int makeBufferedRegion(Buffer    *buffer,
                       size_t     bufferSize,
                       IORegion **regionPtr)
  __attribute__((warn_unused_result));

/**
 * Obtain the buffer of a buffered IO region.
 *
 * @param region        A region created by makeBufferedRegion().
 * @param release       If true, the region will no longer manage any
 *                        buffer it had allocated. Has no effect if the
 *                        region was provided a buffer.
 * @param bufferPtr     Where to store the buffer.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int getBufferedRegionBuffer(IORegion  *region,
                            bool       release,
                            Buffer   **bufferPtr)
  __attribute__((warn_unused_result));

#endif // BUFFERED_IO_REGION_H
