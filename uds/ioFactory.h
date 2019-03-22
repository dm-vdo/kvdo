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
 * $Id: //eng/uds-releases/jasper/src/uds/ioFactory.h#1 $
 */

#ifndef IO_FACTORY_H
#define IO_FACTORY_H

#include "ioFactoryDefs.h"
#include "ioRegion.h"

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
int putIOFactory(IOFactory *factory) __attribute__((warn_unused_result));

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
__attribute__((warn_unused_result))
int makeIORegion(IOFactory  *factory,
                 off_t       offset,
                 size_t      size,
                 IORegion  **regionPtr);

#endif // IO_FACTORY_H
