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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vioRead.h#1 $
 */

#ifndef VIO_READ_H
#define VIO_READ_H

#include "types.h"

/**
 * Start the asynchronous processing of the DataVIO for a read or
 * read-modify-write request which has acquired a lock on its logical block.
 * The first step is to perform a block map lookup.
 *
 * @param dataVIO  The DataVIO doing the read
 **/
void launchReadDataVIO(DataVIO *dataVIO);

/**
 * Clean up a DataVIO which has finished processing a read.
 *
 * @param dataVIO  The DataVIO to clean up
 **/
void cleanupReadDataVIO(DataVIO *dataVIO);

#endif /* VIO_READ_H */
