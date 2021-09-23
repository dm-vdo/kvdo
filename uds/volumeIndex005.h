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
 * $Id: //eng/uds-releases/lisa/src/uds/volumeIndex005.h#1 $
 */

#ifndef VOLUMEINDEX005_H
#define VOLUMEINDEX005_H 1

#include "volumeIndexOps.h"

/**
 * Make a new volume index.
 *
 * @param config        The configuration of the volume index
 * @param volume_nonce  The nonce used to authenticate the index
 * @param volume_index  Location to hold new volume index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check make_volume_index005(const struct configuration *config,
				      uint64_t volume_nonce,
				      struct volume_index **volume_index);

/**
 * Compute the number of bytes required to save a volume index of a given
 * configuration.
 *
 * @param config     The configuration of the volume index
 * @param num_bytes  The number of bytes required to save the volume index
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
compute_volume_index_save_bytes005(const struct configuration *config,
				   size_t *num_bytes);

#endif /* VOLUMEINDEX005_H */
