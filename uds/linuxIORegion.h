/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/linuxIORegion.h#1 $
 */

#ifndef LINUX_IO_REGION_H
#define LINUX_IO_REGION_H

#include "ioFactory.h"
#include "ioRegion.h"

struct block_device;

/**
 * Make an IO region for a block device in the Linux kernel
 *
 * @param [in]  factory     The IOFactory holding the block device
 * @param [in]  bdev        The block device
 * @param [in]  offset      Byte offset to the first block in the block device
 * @param [in]  size        Size of the block device
 * @param [out] regionPtr   Where to put the new region.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int makeLinuxRegion(IOFactory            *factory,
                    struct block_device  *bdev,
                    off_t                 offset,
                    size_t                size,
                    IORegion            **regionPtr)
  __attribute__((warn_unused_result));

#endif // LINUX_IO_REGION_H
