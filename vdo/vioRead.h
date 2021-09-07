/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vioRead.h#4 $
 */

#ifndef VIO_READ_H
#define VIO_READ_H

#include "kernelTypes.h"

/**
 * Start the asynchronous processing of the data_vio for a read or
 * read-modify-write request which has acquired a lock on its logical block.
 * The first step is to perform a block map lookup.
 *
 * @param data_vio  The data_vio doing the read
 **/
void launch_read_data_vio(struct data_vio *data_vio);

/**
 * Clean up a data_vio which has finished processing a read.
 *
 * @param data_vio  The data_vio to clean up
 **/
void cleanup_read_data_vio(struct data_vio *data_vio);

#endif /* VIO_READ_H */
