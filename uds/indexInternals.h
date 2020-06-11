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
 * $Id: //eng/uds-releases/krusty/src/uds/indexInternals.h#7 $
 */

#ifndef INDEX_INTERNALS_H
#define INDEX_INTERNALS_H

#include "index.h"
#include "loadType.h"
#include "request.h"

/**
 * Construct a new index from the given configuration.
 *
 * @param layout       The index layout to use
 * @param config       The configuration to use
 * @param user_params  The index session parameters.  If NULL, the default
 *                     session parameters will be used.
 * @param zone_count   The number of zones for this index to use
 * @param load_type    How to create the index:  it can be create only, allow
 *                     loading from files, and allow rebuilding from the volume
 * @param new_index    A pointer to hold a pointer to the new index
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check allocate_index(struct index_layout *layout,
				const struct configuration *config,
				const struct uds_parameters *user_params,
				unsigned int zone_count,
				enum load_type load_type,
				struct index **new_index);

/**
 * Clean up the index and its memory.
 *
 * @param index    The index to destroy.
 **/
void release_index(struct index *index);

#endif /* INDEX_INTERNALS_H */
