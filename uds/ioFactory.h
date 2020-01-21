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
 * $Id: //eng/uds-releases/jasper/src/uds/ioFactory.h#7 $
 */

#ifndef IO_FACTORY_H
#define IO_FACTORY_H

#include "bufferedReader.h"
#include "bufferedWriter.h"
#ifdef __KERNEL__
#include <linux/dm-bufio.h>
#else
#include "fileUtils.h"
#include "ioRegion.h"
#endif

/*
 * An IOFactory object is responsible for controlling access to index storage.
 * The index is a contiguous range of blocks on a block device or within a
 * file.
 *
 * The IOFactory holds the open device or file and is responsible for closing
 * it.  The IOFactory has methods to make IORegions that are used to access
 * sections of the index.
 */
typedef struct ioFactory IOFactory;

/*
 * Define the UDS block size as 4K.  Historically, we wrote the volume file in
 * large blocks, but wrote all the other index data into byte streams stored in
 * files.  When we converted to writing an index into a block device, we
 * changed to writing the byte streams into page sized blocks.  Now that we
 * support multiple architectures, we write into 4K blocks on all platforms.
 *
 * XXX We must convert all the rogue 4K constants to use UDS_BLOCK_SIZE.
 */
enum { UDS_BLOCK_SIZE = 4096 };

#ifdef __KERNEL__
/**
 * Create an IOFactory.  The IOFactory is returned with a reference count of 1.
 *
 * @param path        The path to the block device or file that contains the
 *                    block stream
 * @param factoryPtr  The IOFactory is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeIOFactory(const char *path, IOFactory **factoryPtr)
  __attribute__((warn_unused_result));
#else
/**
 * Create an IOFactory.  The IOFactory is returned with a reference count of 1.
 *
 * @param path        The path to the block device or file that contains the
 *                    block stream
 * @param access      The requested access kind.
 * @param factoryPtr  The IOFactory is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeIOFactory(const char  *path,
                  FileAccess   access,
                  IOFactory  **factoryPtr)
  __attribute__((warn_unused_result));
#endif

/**
 * Get another reference to an IOFactory, incrementing its reference count.
 *
 * @param factory  The IOFactory
 **/
void getIOFactory(IOFactory *factory);

/**
 * Free a reference to an IOFactory.  If the reference count drops to zero,
 * free the IOFactory and release all its resources.
 *
 * @param factory  The IOFactory
 **/
void putIOFactory(IOFactory *factory);

/**
 * Get the maximum potential size of the device or file.  For a device, this is
 * the actual size of the device.  For a file, this is the largest file that we
 * can possibly write.
 *
 * @param factory  The IOFactory
 *
 * @return the writable size (in bytes)
 **/
size_t getWritableSize(IOFactory *factory) __attribute__((warn_unused_result));

#ifdef __KERNEL__
/**
 * Create a struct dm_bufio_client for a region of the index.
 *
 * @param factory          The IOFactory
 * @param offset           The byte offset to the region within the index
 * @param size             The size of a block, in bytes
 * @param reservedBuffers  The number of buffers that can be reserved
 * @param clientPtr        The struct dm_bufio_client is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeBufio(IOFactory               *factory,
              off_t                    offset,
              size_t                   blockSize,
              unsigned int             reservedBuffers,
              struct dm_bufio_client **clientPtr)
  __attribute__((warn_unused_result));
#else
/**
 * Create an IORegion for a region of the index.
 *
 * @param factory    The IOFactory
 * @param offset     The byte offset to the region within the index
 * @param size       The size in bytes of the region
 * @param regionPtr  The IORegion is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeIORegion(IOFactory  *factory,
                 off_t       offset,
                 size_t      size,
                 IORegion  **regionPtr)
  __attribute__((warn_unused_result));
#endif

/**
 * Create a BufferedReader for a region of the index.
 *
 * @param factory    The IOFactory
 * @param offset     The byte offset to the region within the index
 * @param size       The size in bytes of the region
 * @param regionPtr  The IORegion is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int openBufferedReader(IOFactory       *factory,
                       off_t            offset,
                       size_t           size,
                       BufferedReader **readerPtr)
  __attribute__((warn_unused_result));

/**
 * Create a BufferedWriter for a region of the index.
 *
 * @param factory    The IOFactory
 * @param offset     The byte offset to the region within the index
 * @param size       The size in bytes of the region
 * @param regionPtr  The IORegion is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int openBufferedWriter(IOFactory       *factory,
                       off_t            offset,
                       size_t           size,
                       BufferedWriter **writerPtr)
  __attribute__((warn_unused_result));

#endif // IO_FACTORY_H
