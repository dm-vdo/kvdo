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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/compressionState.c#1 $
 */

#include "compressionStateInternals.h"

#include "dataVIO.h"
#include "packer.h"

static const uint32_t STATUS_MASK           = 0xff;
static const uint32_t MAY_NOT_COMPRESS_MASK = 0x80000000;

/**********************************************************************/
VIOCompressionState getCompressionState(DataVIO *dataVIO)
{
  uint32_t packedValue = atomicLoad32(&dataVIO->compression.state);
  return (VIOCompressionState) {
    .status         = packedValue & STATUS_MASK,
    .mayNotCompress = ((packedValue & MAY_NOT_COMPRESS_MASK) != 0),
  };
}

/**
 * Convert a VIOCompressionState into a uint32_t which may be stored
 * atomically.
 *
 * @param state  The state to convert
 *
 * @return The compression state packed into a uint32_t
 **/
__attribute__((warn_unused_result))
static uint32_t packState(VIOCompressionState state)
{
  return state.status | (state.mayNotCompress ? MAY_NOT_COMPRESS_MASK : 0);
}

/**********************************************************************/
bool setCompressionState(DataVIO             *dataVIO,
                         VIOCompressionState  state,
                         VIOCompressionState  newState)
{
  return compareAndSwap32(&dataVIO->compression.state, packState(state),
                          packState(newState));
}

/**
 * Advance to the next compression state along the compression path.
 *
 * @param dataVIO  The DataVIO to advance
 *
 * @return The new compression status of the DataVIO
 **/
static VIOCompressionStatus advanceStatus(DataVIO *dataVIO)
{
  for (;;) {
    VIOCompressionState state = getCompressionState(dataVIO);
    if (state.status == VIO_POST_PACKER) {
      // We're already in the last state.
      return state.status;
    }

    VIOCompressionState newState = state;
    if (state.mayNotCompress) {
      // Compression has been dis-allowed for this VIO, so skip the rest of the
      // path and go to the end.
      newState.status = VIO_POST_PACKER;
    } else {
      // Go to the next state.
      newState.status++;
    }

    if (setCompressionState(dataVIO, state, newState)) {
      return newState.status;
    }

    // Another thread changed the state out from under us so try again.
  }
}

/**********************************************************************/
bool mayCompressDataVIO(DataVIO *dataVIO)
{
  if (!hasAllocation(dataVIO)
      || ((getWritePolicy(getVDOFromDataVIO(dataVIO)) == WRITE_POLICY_ASYNC)
          && vioRequiresFlushAfter(dataVIOAsVIO(dataVIO)))
      || !getVDOCompressing(getVDOFromDataVIO(dataVIO))) {
    /*
     * If this VIO didn't get an allocation, the compressed write probably
     * won't either, so don't try compressing it. Also, if compression is off,
     * don't compress.
     */
    setCompressionDone(dataVIO);
    return false;
  }

  if (dataVIO->hashLock == NULL) {
    // DataVIOs without a HashLock (which should be extremely rare) aren't
    // able to share the packer's PBN lock, so don't try to compress them.
    return false;
  }

  return (advanceStatus(dataVIO) == VIO_COMPRESSING);
}

/**********************************************************************/
bool mayPackDataVIO(DataVIO *dataVIO)
{
  if (!isSufficientlyCompressible(dataVIO)
      || !getVDOCompressing(getVDOFromDataVIO(dataVIO))
      || getCompressionState(dataVIO).mayNotCompress) {
    // If the data in this VIO doesn't compress, or compression is off, or
    // compression for this VIO has been canceled, don't send it to the packer.
    setCompressionDone(dataVIO);
    return false;
  }

  return true;
}

/**********************************************************************/
bool mayBlockInPacker(DataVIO *dataVIO)
{
  return (advanceStatus(dataVIO) == VIO_PACKING);
}

/**********************************************************************/
bool mayWriteCompressedDataVIO(DataVIO *dataVIO)
{
  advanceStatus(dataVIO);
  return !getCompressionState(dataVIO).mayNotCompress;
}

/**********************************************************************/
void setCompressionDone(DataVIO *dataVIO)
{
  for (;;) {
    VIOCompressionState state = getCompressionState(dataVIO);
    if (state.status == VIO_POST_PACKER) {
      // The VIO is already done.
      return;
    }

    // If compression was cancelled on this VIO, preserve that fact.
    VIOCompressionState newState = {
      .status         = VIO_POST_PACKER,
      .mayNotCompress = true,
    };
    if (setCompressionState(dataVIO, state, newState)) {
      return;
    }
  }
}

/**********************************************************************/
bool cancelCompression(DataVIO *dataVIO)
{
  VIOCompressionState state;
  for (;;) {
    state = getCompressionState(dataVIO);
    if (state.mayNotCompress || (state.status == VIO_POST_PACKER)) {
      // This DataVIO is already set up to not block in the packer.
      break;
    }

    VIOCompressionState newState = {
      .status         = state.status,
      .mayNotCompress = true,
    };
    if (setCompressionState(dataVIO, state, newState)) {
      break;
    }
  }

  return ((state.status == VIO_PACKING) && !state.mayNotCompress);
}
