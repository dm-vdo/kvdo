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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vio-write.h#1 $
 */

#ifndef VIO_WRITE_H
#define VIO_WRITE_H

#include "kernel-types.h"

/**
 * Start the asynchronous processing of a data_vio for a write request which has
 * acquired a lock on its logical block by joining the current flush generation
 * and then attempting to allocate a physical block.
 *
 * @param data_vio  The data_vio doing the write
 **/
void launch_write_data_vio(struct data_vio *data_vio);

/**
 * Clean up a data_vio which has finished processing a write.
 *
 * @param data_vio  The data_vio to clean up
 **/
void cleanup_write_data_vio(struct data_vio *data_vio);

/**
 * Continue a write by attempting to compress the data. This is a re-entry
 * point to vio_write used by hash locks.
 *
 * @param data_vio   The data_vio to be compressed
 **/
void launch_compress_data_vio(struct data_vio *data_vio);

/**
 * Continue a write by deduplicating a write data_vio against a verified
 * existing block containing the data. This is a re-entry point to vio_write
 * used by hash locks.
 *
 * @param data_vio   The data_vio to be deduplicated
 **/
void launch_deduplicate_data_vio(struct data_vio *data_vio);

#endif /* VIO_WRITE_H */
