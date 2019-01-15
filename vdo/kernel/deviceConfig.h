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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/deviceConfig.h#3 $
 */
#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <linux/device-mapper.h>

#include "kernelTypes.h"

// This structure is memcmp'd for equality. Keep it
// packed and don't add any fields that are not
// properly set in both extant and parsed configs.
struct thread_count_config {
  int bioAckThreads;
  int bioThreads;
  int bioRotationInterval;
  int cpuThreads;
  int logicalZones;
  int physicalZones;
  int hashZones;
} __attribute__((packed));

typedef uint32_t TableVersion;

struct device_config {
  struct dm_target           *owningTarget;
  struct dm_dev              *ownedDevice;
  KernelLayer                *layer;
  char                       *originalString;
  TableVersion                version;
  char                       *parentDeviceName;
  BlockCount                  physicalBlocks;
  unsigned int                logicalBlockSize;
  WritePolicy                 writePolicy;
  unsigned int                cacheSize;
  unsigned int                blockMapMaximumAge;
  bool                        mdRaid5ModeEnabled;
  char                       *poolName;
  struct thread_count_config  threadCounts;
  BlockCount                  maxDiscardBlocks;
};

/**
 * Grab a pointer to the pool name out of argv.
 *
 * @param [in]  argc         The number of table values
 * @param [in]  argv         The array of table values
 * @param [out] errorPtr     A pointer to return a error string in
 * @param [out] poolNamePtr  A pointer to return the pool name
 *
 * @return VDO_SUCCESS or an error code
 **/
int get_pool_name_from_argv(int    argc,
                        char **argv,
                        char **errorPtr,
                        char **poolNamePtr)
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
int parse_device_config(int                    argc,
                        char                 **argv,
                        struct dm_target      *ti,
                        bool                   verbose,
                        struct device_config **configPtr)
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

#endif // DEVICE_CONFIG_H
