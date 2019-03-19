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
 * $Id: //eng/uds-releases/gloria/src/uds/blockIORegion.h#1 $
 */

#ifndef BLOCK_IO_REGION_H
#define BLOCK_IO_REGION_H

#include "ioRegion.h"

#include "accessMode.h"

/**
 * Obtain access to a block-based subregion of an IORegion.
 *
 * @param [in]  parent          An underlying region to read and write from.
 * @param [in]  access          Access type (must be compatible with that of
 *                                underlying region).
 * @param [in]  start           Start of data region.
 * @param [in]  end             End of data region.
 * @param [out] regionPtr       Where to put the new region.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int openBlockRegion(IORegion       *parent,
                    IOAccessMode    access,
                    off_t           start,
                    off_t           end,
                    IORegion      **regionPtr)
  __attribute__((warn_unused_result));

#endif // BLOCK_IO_REGION_H
