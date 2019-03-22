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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/ioFactoryLinuxKernel.c#1 $
 */

#include <linux/blkdev.h>
#include <linux/mount.h>

#include "atomicDefs.h"
#include "ioFactory.h"
#include "linuxIORegion.h"
#include "logger.h"
#include "memoryAlloc.h"

enum { BLK_FMODE = FMODE_READ | FMODE_WRITE };

/*
 * A kernel mode IOFactory object controls access to an index stored on a block
 * device.
 */
struct ioFactory {
  struct block_device *bdev;
  atomic_t             refCount;
};

/*****************************************************************************/
void getIOFactory(IOFactory *factory)
{
  atomic_inc(&factory->refCount);
}

/*****************************************************************************/
int makeIOFactory(const char *path, IOFactory **factoryPtr)
{
  struct block_device *bdev;
  dev_t device = name_to_dev_t(path);
  if (device != 0) {
    bdev = blkdev_get_by_dev(device, BLK_FMODE, NULL);
  } else {
    bdev = blkdev_get_by_path(path, BLK_FMODE, NULL);
  }
  if (IS_ERR(bdev)) {
    logErrorWithStringError(-PTR_ERR(bdev), "%s is not a block device", path);
    return UDS_INVALID_ARGUMENT;
  }

  IOFactory *factory;
  int result = ALLOCATE(1, IOFactory, __func__, &factory);
  if (result != UDS_SUCCESS) {
    blkdev_put(bdev, BLK_FMODE);
    return result;
  }

  factory->bdev = bdev;
  atomic_set_release(&factory->refCount, 1);

  *factoryPtr = factory;
  return UDS_SUCCESS;
}

/*****************************************************************************/
int putIOFactory(IOFactory *factory)
{
  if (atomic_add_return(-1, &factory->refCount) > 0) {
    return UDS_SUCCESS;
  }
  blkdev_put(factory->bdev, BLK_FMODE);
  FREE(factory);
  return UDS_SUCCESS;
}

/*****************************************************************************/
int makeIORegion(IOFactory  *factory,
                 off_t       offset,
                 size_t      size,
                 IORegion  **regionPtr)
{
  IORegion *region;
  int result = makeLinuxRegion(factory, factory->bdev, offset, size, &region);
  if (result != UDS_SUCCESS) {
    return result;
  }


  *regionPtr = region;
  return result;
}
