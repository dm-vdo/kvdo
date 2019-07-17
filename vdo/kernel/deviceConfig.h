/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/deviceConfig.h#7 $
 */
#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <linux/device-mapper.h>

#include "ringNode.h"

#include "kernelTypes.h"

/*
 * This structure is memcmp'd for equality. Keep it packed and don't
 * add any fields that are not properly set in both extant and parsed
 * configs.
 */
struct thread_count_config {
	int bio_ack_threads;
	int bio_threads;
	int bio_rotation_interval;
	int cpu_threads;
	int logical_zones;
	int physical_zones;
	int hash_zones;
} __attribute__((packed));

typedef uint32_t TableVersion;

struct device_config {
	struct dm_target *owning_target;
	struct dm_dev *owned_device;
	struct kernel_layer *layer;
	/** All configs referencing a layer are kept on a ring in the layer */
	RingNode config_node;
	char *original_string;
	TableVersion version;
	char *parent_device_name;
	BlockCount physical_blocks;
	unsigned int logical_block_size;
	WritePolicy write_policy;
	unsigned int cache_size;
	unsigned int block_map_maximum_age;
	bool md_raid5_mode_enabled;
	char *pool_name;
	struct thread_count_config thread_counts;
	BlockCount max_discard_blocks;
};

/**
 * Convert a RingNode to the device_config that contains it.
 *
 * @param node  The RingNode to convert
 *
 * @return The device_config wrapping the RingNode
 **/
static inline struct device_config *as_device_config(RingNode *node)
{
	if (node == NULL) {
		return NULL;
	}
	return (struct device_config *)
		((byte *) node - offsetof(struct device_config, config_node));
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
int get_pool_name_from_argv(int argc,
			    char **argv,
			    char **error_ptr,
			    char **pool_name_ptr)
	__attribute__((warn_unused_result));

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
int parse_device_config(int argc,
			char **argv,
			struct dm_target *ti,
			bool verbose,
			struct device_config **config_ptr)
	__attribute__((warn_unused_result));

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
const char *get_config_write_policy_string(struct device_config *config)
	__attribute__((warn_unused_result));

/**
 * Acquire or release a reference from the config to a kernel layer.
 *
 * @param config  The config in question
 * @param layer   The kernel layer in question
 **/
void set_device_config_layer(struct device_config *config,
			     struct kernel_layer *layer);

#endif // DEVICE_CONFIG_H
