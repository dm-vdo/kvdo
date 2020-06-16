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
 * $Id: //eng/uds-releases/krusty/src/uds/masterIndex006.h#5 $
 */

#ifndef MASTERINDEX006_H
#define MASTERINDEX006_H 1

#include "masterIndexOps.h"

/**
 * Make a new master index.
 *
 * @param config          The configuration of the master index
 * @param num_zones       The number of zones
 * @param volume_nonce    The nonce used to authenticate the index
 * @param master_index    Location to hold new master index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check make_master_index006(const struct configuration *config,
				      unsigned int num_zones,
				      uint64_t volume_nonce,
				      struct master_index **master_index);

/**
 * Compute the number of bytes required to save a master index of a given
 * configuration.
 *
 * @param config     The configuration of the master index
 * @param num_bytes  The number of bytes required to save the master index
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
compute_master_index_save_bytes006(const struct configuration *config,
				   size_t *num_bytes);

#endif /* MASTERINDEX006_H */
