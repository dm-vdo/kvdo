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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/deviceConfig.h#20 $
 */
#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <linux/device-mapper.h>
#include <linux/list.h>

#include "kernelTypes.h"

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
	struct kernel_layer *layer;
	/** All configs referencing a layer are kept on a list in the layer */
	struct list_head config_list;
	char *original_string;
	unsigned int version;
	char *parent_device_name;
	block_count_t physical_blocks;
	unsigned int logical_block_size;
	enum write_policy write_policy;
	unsigned int cache_size;
	unsigned int block_map_maximum_age;
	bool deduplication;
	char *pool_name;
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
static inline struct device_config *as_device_config(struct list_head *entry)
{
	if (entry == NULL) {
		return NULL;
	}
	return list_entry(entry, struct device_config, config_list);
}

/**
 * Grab a pointer to the pool name out of argv.
 *
 * @param [in]  argc           The number of table values
 * @param [in]  argv           The array of table values
 * @param [out] error_ptr      A pointer to return a error string in
 * @param [out] pool_name_ptr  A pointer to return the pool name
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
get_pool_name_from_argv(int argc,
			char **argv,
			char **error_ptr,
			char **pool_name_ptr);

/**
 * Convert the dmsetup table into a struct device_config.
 *
 * @param [in]  argc         The number of table values
 * @param [in]  argv         The array of table values
 * @param [in]  ti           The target structure for this table
 * @param [in]  verbose      Whether to log about the underlying device
 * @param [out] config_ptr   A pointer to return the allocated config
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check parse_device_config(int argc,
				     char **argv,
				     struct dm_target *ti,
				     bool verbose,
				     struct device_config **config_ptr);

/**
 * Free a device config created by parse_device_config().
 *
 * @param config_ptr  The pointer holding the config, which will be nulled
 **/
void free_device_config(struct device_config **config_ptr);

/**
 * Get the text describing the write policy.
 *
 * @param config  The device config
 *
 * @returns a pointer to a string describing the write policy
 **/
const char * __must_check
get_config_write_policy_string(struct device_config *config);

/**
 * Acquire or release a reference from the config to a kernel layer.
 *
 * @param config  The config in question
 * @param layer   The kernel layer in question
 **/
void set_device_config_layer(struct device_config *config,
			     struct kernel_layer *layer);

#endif // DEVICE_CONFIG_H
