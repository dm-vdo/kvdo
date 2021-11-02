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
 */
#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <linux/device-mapper.h>
#include <linux/list.h>

#include "types.h"

#include "kernel-types.h"

/*
 * This structure is memcmp'd for equality. Keep it packed and don't add any
 * fields that are not properly set in both extant and parsed configs.
 */
struct thread_count_config {
	int bio_ack_threads;
	int bio_threads;
	int bio_rotation_interval;
	int cpu_threads;
	int logical_zones;
	int physical_zones;
	int hash_zones;
} __packed;

struct device_config {
	struct dm_target *owning_target;
	struct dm_dev *owned_device;
	struct vdo *vdo;
	/** All configs referencing a layer are kept on a list in the layer */
	struct list_head config_list;
	char *original_string;
	unsigned int version;
	char *parent_device_name;
	block_count_t physical_blocks;
	/**
	 * This is the number of logical blocks from VDO's internal point of
	 * view. It is the number of 4K blocks regardles of the value of the
	 * logical_block_size parameter below.
	 **/
	block_count_t logical_blocks;
	unsigned int logical_block_size;
	unsigned int cache_size;
	unsigned int block_map_maximum_age;
	bool deduplication;
	bool compression;
	struct thread_count_config thread_counts;
	block_count_t max_discard_blocks;
};

/**
 * Convert a list entry to the device_config that contains it. If non-NULL,
 * the list must not be empty.
 *
 * @param entry  The list entry to convert
 *
 * @return The device_config wrapping the list entry
 **/
static inline struct device_config *as_vdo_device_config(struct list_head *entry)
{
	if (entry == NULL) {
		return NULL;
	}
	return list_entry(entry, struct device_config, config_list);
}

/**
 * Convert the dmsetup table into a struct device_config.
 *
 * @param [in]  argc        The number of table values
 * @param [in]  argv        The array of table values
 * @param [in]  ti          The target structure for this table
 * @param [out] config_ptr  A pointer to return the allocated config
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check parse_vdo_device_config(int argc,
					 char **argv,
					 struct dm_target *ti,
					 struct device_config **config_ptr);

/**
 * Free a device config created by parse_vdo_device_config().
 *
 * @param config  The config to free
 **/
void free_vdo_device_config(struct device_config *config);

/**
 * Acquire or release a reference from the config to a vdo.
 *
 * @param config  The config in question
 * @param vdo     The vdo in question
 **/
void set_device_config_vdo(struct device_config *config, struct vdo *vdo);

/**
 * Check whether a new device config represents a valid modification to an
 * existing config.
 *
 * @param to_validate  The new config to valudate
 * @param config       The existing config
 * @param may_grow     Set to true if growing the logical and physical size of
 *                     the vdo is currently permitted
 * @param error_ptr    A pointer to hold the reason for any error
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
validate_new_vdo_device_config(struct device_config *to_validate,
			       struct device_config *config,
			       bool may_grow,
			       char **error_ptr);

#endif /* DEVICE_CONFIG_H */
