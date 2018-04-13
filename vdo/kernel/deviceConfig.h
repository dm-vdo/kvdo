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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/deviceConfig.h#1 $
 */
#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include "kernelTypes.h"

typedef struct {
  int baseThreads;
  int bioAckThreads;
  int bioThreads;
  int bioRotationInterval;
  int cpuThreads;
  int logicalZones;
  int physicalZones;
  int hashZones;
} ThreadCountConfig;

typedef struct {
  char              *originalString;
  char              *parentDeviceName;
  unsigned int       logicalBlockSize;
  WritePolicy        writePolicy;
  unsigned int       cacheSize;
  unsigned int       blockMapMaximumAge;
  bool               mdRaid5ModeEnabled;
  bool               readCacheEnabled;
  unsigned int       readCacheExtraBlocks;
  char              *poolName;
  char              *threadConfigString;
  ThreadCountConfig  threadCounts;
} DeviceConfig;

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
int getPoolNameFromArgv(int    argc,
                        char **argv,
                        char **errorPtr,
                        char **poolNamePtr)
  __attribute__((warn_unused_result));

/**
 * Convert the dmsetup argument list into a DeviceConfig.
 *
 * @param [in]  argc        The number of table values
 * @param [in]  argv        The array of table values
 * @param [out] errorPtr    A pointer to return a error string in
 * @param [out] configPtr   A pointer to return the allocated config
 *
 * @return VDO_SUCCESS or an error code
 **/
int parseDeviceConfig(int            argc,
                      char         **argv,
                      char         **errorPtr,
                      DeviceConfig **configPtr)
  __attribute__((warn_unused_result));

/**
 * Free a device config created by parseDeviceConfig().
 *
 * @param configPtr  The pointer holding the config, which will be nulled
 **/
void freeDeviceConfig(DeviceConfig **configPtr);

/**
 * Get the text describing the write policy.
 *
 * @param config  The device config
 *
 * @returns a pointer to a string describing the write policy
 **/
const char *getConfigWritePolicyString(DeviceConfig *config)
  __attribute__((warn_unused_result));

/**
 * Resolve the write policy if needed.
 *
 * @param [in,out] config          The config to resolve
 * @param [in]     flushSupported  Whether flushes are supported.
 **/
void resolveConfigWithFlushSupport(DeviceConfig *config, bool flushSupported);

#endif // DEVICE_CONFIG_H
