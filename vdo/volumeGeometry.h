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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/volumeGeometry.h#18 $
 */

#ifndef VOLUME_GEOMETRY_H
#define VOLUME_GEOMETRY_H

#include "uds.h"

#include "types.h"

struct index_config {
	uint32_t mem;
	uint32_t checkpoint_frequency;
	bool sparse;
} __packed;

typedef enum {
	INDEX_REGION = 0,
	DATA_REGION = 1,
	VOLUME_REGION_COUNT,
} volume_region_id;

struct volume_region {
	/** The ID of the region */
	volume_region_id id;
	/**
	 * The absolute starting offset on the device. The region continues
	 * until the next region begins.
	 */
	physical_block_number_t start_block;
} __packed;

/** A binary UUID is 16 bytes. */
typedef unsigned char UUID[16];

struct volume_geometry {
	/** The release version number of this volume */
	release_version_number_t release_version;
	/** The nonce of this volume */
	nonce_t nonce;
	/** The UUID of this volume */
	UUID uuid;
	/** The regions in ID order */
	struct volume_region regions[VOLUME_REGION_COUNT];
	/** The index config */
	struct index_config index_config;
} __packed;

/**
 * Get the start of the index region from a geometry.
 *
 * @param geometry  The geometry
 *
 * @return The start of the index region
 **/
static inline physical_block_number_t __must_check
get_index_region_offset(struct volume_geometry geometry)
{
	return geometry.regions[INDEX_REGION].start_block;
}

/**
 * Get the start of the data region from a geometry.
 *
 * @param geometry  The geometry
 *
 * @return The start of the data region
 **/
static inline physical_block_number_t __must_check
get_data_region_offset(struct volume_geometry geometry)
{
	return geometry.regions[DATA_REGION].start_block;
}

/**
 * Get the size of the index region from a geometry.
 *
 * @param geometry  The geometry
 *
 * @return the size of the index region
 **/
static inline physical_block_number_t __must_check
get_index_region_size(struct volume_geometry geometry)
{
	return get_data_region_offset(geometry) -
		get_index_region_offset(geometry);
}

/**
 * Read the volume geometry from a layer.
 *
 * @param layer     The layer to read and parse the geometry from
 * @param geometry  The geometry to be loaded
 **/
int __must_check
load_volume_geometry(PhysicalLayer *layer, struct volume_geometry *geometry);


/**
 * Convert an index config to a UDS configuration, which can be used by UDS.
 *
 * @param index_config    The index config to convert
 * @param uds_config_ptr  A pointer to return the UDS configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
index_config_to_uds_configuration(const struct index_config *index_config,
				  struct uds_configuration **uds_config_ptr);

/**
 * Modify the uds_parameters to match the requested index config.
 *
 * @param index_config  The index config to convert
 * @param user_params   The uds_parameters to modify
 **/
void index_config_to_uds_parameters(const struct index_config *index_config,
				    struct uds_parameters *user_params);


/**
 * Set load config fields from a volume geometry.
 *
 * @param geometry     The geometry to use
 * @param load_config  The load config to set
 **/
static inline void
set_load_config_from_geometry(struct volume_geometry *geometry,
			      struct vdo_load_config *load_config)
{
	load_config->first_block_offset = get_data_region_offset(*geometry);
	load_config->release_version = geometry->release_version;
	load_config->nonce = geometry->nonce;
}

#endif // VOLUME_GEOMETRY_H
