/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/src/uds/ioRegion.h#2 $
 */

#ifndef IO_REGION_H
#define IO_REGION_H

#include "typeDefs.h"

/**
 * The IORegion type is an abstraction which represents a specific place
 * which can be read or written. There are file-based implementations as
 * well as block-range based implementations. Although the operations
 * defined on IORegion appear to take any byte address in reality these
 * addresses can be constrained to the implementation's alignment
 * restrictions.
 **/
typedef struct ioRegion IORegion;

/**
 * Close an IO region and set the region pointer to NULL.
 *
 * @param [in, out] regionPtr   An IORegion.
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int closeIORegion(IORegion **regionPtr);

/**
 * Get the predefined size of the region. Note not all implementations have
 * a limit.
 *
 * @param [in]  region          The IORegion.
 * @param [out] limit           The maximum offset of the region in bytes,
 *                                set to the maximum value if unlimited.
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int getRegionLimit(IORegion *region,
                          off_t    *limit)
  __attribute__((warn_unused_result));

/**
 * Get the extent of of previously written data. Note that not all regions
 * can track this information, some just return the limit.
 *
 * @param [in]  region          The IORegion.
 * @param [out] extent          The maximum offset of the existing data
 *                                in the region, set to limit if unknown.
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int getRegionDataSize(IORegion *region,
                             off_t    *extent)
  __attribute__((warn_unused_result));

/**
 * Clear the region.
 *
 * @param region                The IORegion.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int clearRegion(IORegion *region)
  __attribute__((warn_unused_result));

/**
 * Write a buffer to a region.
 *
 * @param region                The IORegion.
 * @param offset                The offset at which to write; must be
 *                                aligned to the region's block size.
 * @param data                  A buffer of data.
 * @param size                  The size of the buffer; must be a multiple
 *                                of the block size.
 * @param length                The length of the data. May be shorter than
 *                                the size of the buffer. It is implementation-
 *                                specific whether the region supports short
 *                                writes, so the entire buffer may be written.
 *
 * @return UDS_SUCCESS or an error code, potentially
 *         UDS_INCORRECT_ALIGNMENT if the offset is incorrect,
 *         UDS_BUFFER_ERROR if the buffer size is incorrect,
 *         UDS_OUT_OF_RANGE if the offset plus length exceeds the region limits
 **/
static int writeToRegion(IORegion   *region,
                         off_t       offset,
                         const void *data,
                         size_t      size,
                         size_t      length)
  __attribute__((warn_unused_result));

/**
 * Read some data from a region into a buffer.
 *
 * @param [in]      region      The IORegion.
 * @param [in]      offset      The offset from which to read; must be
 *                                aligned to the regions's block size.
 * @param [out]     buffer      The buffer to read to.
 * @param [in]      size        The size of the data buffer; must be a
 *                                multiple of the block size.
 * @param [in, out] length      If non-NULL, allow partial reads by specifying
 *                                the minimum length of read required. Reads
 *                                shorter than that specified are an error.
 *                                Upon return, the actual length of the data
 *                                in the buffer is stored. If NULL the required
 *                                length is presumed to be the entire buffer.
 *
 * @return UDS_SUCCESS or an error code, potentially
 *         UDS_BUFFER_ERROR if the buffer size is incorrect,
 *         UDS_INCORRECT_ALIGNMENT if the offset is incorrect,
 *         UDS_END_OF_FILE or UDS_SHORT_READ if the data is not available,
 **/
static int readFromRegion(IORegion *region,
                          off_t     offset,
                          void     *buffer,
                          size_t    size,
                          size_t   *length)
  __attribute__((warn_unused_result));

/**
 * Obtain the block size for the region. The block size constrains the
 * alignment of the offsets as well as the the size of the buffers used
 * in reading and writing, which must be a multiple of the block size.
 *
 * @param [in]  region          The IORegion.
 * @param [out] blockSize       The block size for this region.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int getRegionBlockSize(IORegion *region,
                              size_t   *blockSize)
  __attribute__((warn_unused_result));

/**
 * Obtain the most efficient buffer size for IO.
 *
 * @param [in]  region          The IORegion.
 * @param [out] bufferSize      The best size for IO buffers. Will be
 *                                a multiple (possibly 1) of the region's
 *                                block size.
 **/
static int getRegionBestBufferSize(IORegion *region,
                                   size_t   *bufferSize)
  __attribute__((warn_unused_result));

/**
 * Force the region to be written to the backing store, if supported.
 *
 * @param region                The IORegion.
 *
 * @return UDS_SUCCESS or an error code, particularly UDS_UNSUPPORTED for
 *         regions where this operation is not implemented.
 **/
static int syncRegionContents(IORegion *region)
  __attribute__((warn_unused_result));

/**
 * Sync the region contents and then close the region. This is a utility
 * wrapper.
 *
 * @param regionPtr             A pointer to the region.
 * @param failureMsg            A message to be logged on failure.
 *
 * @return UDS_SUCCESS or an error code. Note the UDS_UNSUPPORTED error
 *         from syncRegionContents() is silently swallowed.
 **/
int syncAndCloseRegion(IORegion **regionPtr, const char *failureMsg);

// Dull, boring implementation details below...

#define IO_REGION_INLINE
# include "ioRegionInline.h"
#undef IO_REGION_INLINE

#endif // IO_REGION_H
