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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/compressionState.h#2 $
 */

#ifndef COMPRESSION_STATE_H
#define COMPRESSION_STATE_H

#include "atomic.h"
#include "types.h"

/**
 * Where a DataVIO is on the compression path; advanceStatus() depends on the
 * order of this enum.
 **/
typedef enum {
  /* A VIO which has not yet entered the compression path */
  VIO_PRE_COMPRESSOR = 0,
  /* A VIO which is in the compressor */
  VIO_COMPRESSING,
  /* A VIO which is blocked in the packer */
  VIO_PACKING,
  /* A VIO which is no longer on the compression path (and never will be) */
  VIO_POST_PACKER,
} VIOCompressionStatus;

typedef struct {
  VIOCompressionStatus status;
  bool                 mayNotCompress;
} VIOCompressionState;

/**
 * Get the compression state of a DataVIO.
 *
 * @param dataVIO  The DataVIO
 *
 * @return The compression state
 **/
__attribute__((warn_unused_result))
VIOCompressionState getCompressionState(DataVIO *dataVIO);

/**
 * Check whether a DataVIO may go to the compressor.
 *
 * @param dataVIO  The DataVIO to check
 *
 * @return <code>true</code> if the DataVIO may be compressed at this time
 **/
bool mayCompressDataVIO(DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Check whether a DataVIO may go to the packer.
 *
 * @param dataVIO  The DataVIO to check
 *
 * @return <code>true</code> if the DataVIO may be packed at this time
 **/
bool mayPackDataVIO(DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Check whether a DataVIO which has gone to the packer may block there. Any
 * cancelation after this point and before the DataVIO is written out requires
 * this DataVIO to be picked up by the canceling DataVIO.
 *
 * @param dataVIO  The DataVIO to check
 *
 * @return <code>true</code> if the DataVIO may block in the packer
 **/
bool mayBlockInPacker(DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Check whether the packer may write out a DataVIO as part of a compressed
 * block.
 *
 * @param dataVIO  The DataVIO to check
 *
 * @return <code>true</code> if the DataVIO may be written as part of a
 *         compressed block at this time
 **/
bool mayWriteCompressedDataVIO(DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Indicate that this DataVIO is leaving the compression path.
 *
 * @param dataVIO  The DataVIO leaving the compression path
 **/
void setCompressionDone(DataVIO *dataVIO);

/**
 * Prevent this DataVIO from being compressed or packed.
 *
 * @param dataVIO  The DataVIO to cancel
 *
 * @return <code>true</code> if the DataVIO is in the packer and the caller
 *         was the first caller to cancel it
 **/
bool cancelCompression(DataVIO *dataVIO);

#endif /* COMPRESSION_STATE_H */
