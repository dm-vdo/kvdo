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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vioWrite.h#1 $
 */

#ifndef VIO_WRITE_H
#define VIO_WRITE_H

#include "types.h"

/**
 * Release the PBN read lock if it is held.
 *
 * @param dataVIO  The possible lock holder
 **/
void releasePBNReadLock(DataVIO *dataVIO);

/**
 * Start the asynchronous processing of a DataVIO for a write request which has
 * acquired a lock on its logical block by joining the current flush generation
 * and then attempting to allocate a physical block.
 *
 * @param dataVIO  The DataVIO doing the write
 **/
void launchWriteDataVIO(DataVIO *dataVIO);

/**
 * Clean up a DataVIO which has finished processing a write.
 *
 * @param dataVIO  The DataVIO to clean up
 **/
void cleanupWriteDataVIO(DataVIO *dataVIO);

/**
 * Continue a write by attempting to compress the data. This is a re-entry
 * point to vioWrite used by hash locks.
 *
 * @param dataVIO   The DataVIO to be compressed
 **/
void compressData(DataVIO *dataVIO);

#endif /* VIO_WRITE_H */
