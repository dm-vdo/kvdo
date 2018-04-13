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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/deviceConfig.c#1 $
 */

#include "deviceConfig.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

#include "vdoStringUtils.h"

enum {
  REQUIRED_ARGC                  = 10,
  POOL_NAME_ARG_INDEX            = 8,
  // Arbitrary limit used when parsing thread-count config spec strings
  THREAD_COUNT_LIMIT             = 100,
  BIO_ROTATION_INTERVAL_LIMIT    = 1024,
  // XXX The bio-submission queue configuration defaults are temporarily
  // still being defined here until the new runtime-based thread
  // configuration has been fully implemented for managed VDO devices.

  // How many bio submission work queues to use
  DEFAULT_NUM_BIO_SUBMIT_QUEUES             = 4,
  // How often to rotate between bio submission work queues
  DEFAULT_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL  = 64,
};

/**********************************************************************/
int getPoolNameFromArgv(int    argc,
                        char **argv,
                        char **errorPtr,
                        char **poolNamePtr)
{
  if (argc != REQUIRED_ARGC) {
    *errorPtr = "Incorrect number of arguments";
    return VDO_BAD_CONFIGURATION;
  }
  *poolNamePtr = argv[POOL_NAME_ARG_INDEX];
  return VDO_SUCCESS;
}

/**
 * Parse a two-valued option into a bool.
 *
 * @param [in]  boolStr    The string value to convert to a bool
 * @param [in]  trueStr    The string value which should be converted to true
 * @param [in]  falseStr   The string value which should be converted to false
 * @param [out] boolPtr    A pointer to return the bool value in
 *
 * @return VDO_SUCCESS or an error if boolStr is neither trueStr nor falseStr
 **/
__attribute__((warn_unused_result))
static inline int parseBool(const char *boolStr,
                            const char *trueStr,
                            const char *falseStr,
                            bool       *boolPtr)
{
  bool value = false;
  if (strcmp(boolStr, trueStr) == 0) {
    value = true;
  } else if (strcmp(boolStr, falseStr) == 0) {
    value = false;
  } else {
    return VDO_BAD_CONFIGURATION;
  }

  *boolPtr = value;
  return VDO_SUCCESS;
}

/**
 * Process one component of a thread parameter configuration string and
 * update the configuration data structure.
 *
 * If the thread count requested is invalid, a message is logged and
 * -EINVAL returned.
 *
 * @param threadParamType  The type of thread specified
 * @param count            The thread count requested
 * @param config           The configuration data structure to update
 *
 * @return   VDO_SUCCESS or -EINVAL
 **/
static int processOneThreadConfigSpec(const char        *threadParamType,
                                      unsigned int       count,
                                      ThreadCountConfig *config)
{
  // Handle thread parameters other than thread config
  if (strcmp(threadParamType, "bioRotationInterval") == 0) {
    if (count == 0) {
      logError("thread config string error:"
               " 'bioRotationInterval' of at least 1 is required");
      return -EINVAL;
    } else if (count > BIO_ROTATION_INTERVAL_LIMIT) {
      logError("thread config string error:"
               " 'bioRotationInterval' cannot be higher than %d",
               BIO_ROTATION_INTERVAL_LIMIT);
      return -EINVAL;
    }
    config->bioRotationInterval = count;
    return VDO_SUCCESS;
  } else {
    // Handle thread count parameters
    if (count > THREAD_COUNT_LIMIT) {
      logError("thread config string error: at most %d '%s' threads"
               " are allowed",
               THREAD_COUNT_LIMIT, threadParamType);
      return -EINVAL;
    }

    if (strcmp(threadParamType, "cpu") == 0) {
      if (count == 0) {
        logError("thread config string error:"
                 " at least one 'cpu' thread required");
        return -EINVAL;
      }
      config->cpuThreads = count;
      return VDO_SUCCESS;
    } else if (strcmp(threadParamType, "ack") == 0) {
      config->bioAckThreads = count;
      return VDO_SUCCESS;
    } else if (strcmp(threadParamType, "bio") == 0) {
      if (count == 0) {
        logError("thread config string error:"
                 " at least one 'bio' thread required");
        return -EINVAL;
      }
      config->bioThreads = count;
      return VDO_SUCCESS;
    } else if (strcmp(threadParamType, "logical") == 0) {
      config->logicalZones = count;
      return VDO_SUCCESS;
    } else if (strcmp(threadParamType, "physical") == 0) {
      config->physicalZones = count;
      return VDO_SUCCESS;
    } else if (strcmp(threadParamType, "hash") == 0) {
      config->hashZones = count;
      return VDO_SUCCESS;
    }
  }

  // More will be added eventually.
  logError("unknown thread parameter type \"%s\"", threadParamType);
  return -EINVAL;
}

/**
 * Parse one component of a thread parameter configuration string and
 * update the configuration data structure.
 *
 * @param spec    The thread parameter specification string
 * @param config  The configuration data to be updated
 **/
static int parseOneThreadConfigSpec(const char        *spec,
                                    ThreadCountConfig *config)
{
  char **fields;
  int result = splitString(spec, '=', &fields);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if ((fields[0] == NULL) || (fields[1] == NULL) || (fields[2] != NULL)) {
    logError("thread config string error:"
             " expected thread parameter assignment, saw \"%s\"",
             spec);
    freeStringArray(fields);
    return -EINVAL;
  }

  unsigned int count;
  result = stringToUInt(fields[1], &count);
  if (result != UDS_SUCCESS) {
    logError("thread config string error: integer value needed, found \"%s\"",
             fields[1]);
    freeStringArray(fields);
    return result;
  }

  result = processOneThreadConfigSpec(fields[0], count, config);
  freeStringArray(fields);
  return result;
}

/**
 * Parse the configuration string passed and update the specified
 * counts and other parameters of various types of threads to be created.
 *
 * The configuration string should contain one or more comma-separated specs
 * of the form "typename=number"; the supported type names are "cpu", "ack",
 * "bio", "bioRotationInterval", "logical", "physical", and "hash".
 *
 * An incorrect format or unknown thread parameter type name is an error.
 *
 * This function can't set the "reason" value the caller wants to pass
 * back, because we'd want to format it to say which field was
 * invalid, and we can't allocate the "reason" strings dynamically. So
 * if an error occurs, we'll just log the details, and pass back
 * EINVAL.
 *
 * @param string  Thread parameter configuration string
 * @param config  The thread configuration data to update
 *
 * @return   VDO_SUCCESS or -EINVAL or -ENOMEM
 **/
static int parseThreadConfigString(const char        *string,
                                   ThreadCountConfig *config)
{
  char **specs;
  int result = splitString(string, ',', &specs);
  if (result != UDS_SUCCESS) {
    return result;
  }
  for (unsigned int i = 0; specs[i] != NULL; i++) {
    result = parseOneThreadConfigSpec(specs[i], config);
    if (result != VDO_SUCCESS) {
      break;
    }
  }
  freeStringArray(specs);
  return result;
}

/**
 * Handle a parsing error.
 *
 * @param configPtr     A pointer to the config to free
 * @param errorPtr      A place to store a constant string about the error
 * @param errorStr      A constant string to store in errorPtr
 **/
static void handleParseError(DeviceConfig **configPtr,
                             char         **errorPtr,
                             char          *errorStr)
{
  freeDeviceConfig(configPtr);
  *errorPtr = errorStr;
}

/**********************************************************************/
int parseDeviceConfig(int            argc,
                      char         **argv,
                      char         **errorPtr,
                      DeviceConfig **configPtr)
{
  DeviceConfig *config = NULL;
  if (argc != REQUIRED_ARGC) {
    handleParseError(&config, errorPtr, "Incorrect number of arguments");
    return VDO_BAD_CONFIGURATION;
  }

  int result = ALLOCATE(1, DeviceConfig, "DeviceConfig", &config);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Could not allocate config structure");
    return VDO_BAD_CONFIGURATION;
  }

  // Save the original string.
  result = joinStrings(argv, argc, ' ', &config->originalString);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Could not populate string");
    return VDO_BAD_CONFIGURATION;
  }

  // Set defaults.
  //
  // XXX Defaults for bioThreads and bioRotationInterval are currently defined
  // using the old configuration scheme of constants.  These values are relied
  // upon for performance testing on MGH machines currently.
  // This should be replaced with the normally used testing defaults being
  // defined in the file-based thread-configuration settings.  The values used
  // as defaults internally should really be those needed for VDO in its
  // default shipped-product state.
  config->threadCounts = (ThreadCountConfig) {
    .bioAckThreads       = 1,
    .bioThreads          = DEFAULT_NUM_BIO_SUBMIT_QUEUES,
    .bioRotationInterval = DEFAULT_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL,
    .cpuThreads          = 1,
    .logicalZones        = 0,
    .physicalZones       = 0,
    .hashZones           = 0,
  };

  char **argumentPtr = argv;

  result = duplicateString(*argumentPtr++, "parent device name",
                           &config->parentDeviceName);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Could not copy parent device name");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the logical block size and validate
  bool enable512e;
  result = parseBool(*argumentPtr++, "512", "4096", &enable512e);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Invalid logical block size");
    return VDO_BAD_CONFIGURATION;
  }
  config->logicalBlockSize = (enable512e ? 512 : 4096);

  // Determine whether the read cache is enabled.
  result = parseBool(*argumentPtr++, "enabled", "disabled",
                     &config->readCacheEnabled);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Invalid read cache mode");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the number of extra blocks for the read cache.
  result = stringToUInt(*argumentPtr++, &config->readCacheExtraBlocks);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr,
                     "Invalid read cache extra block count");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the page cache size.
  result = stringToUInt(*argumentPtr++, &config->cacheSize);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Invalid block map page cache size");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the block map era length.
  result = stringToUInt(*argumentPtr++, &config->blockMapMaximumAge);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Invalid block map maximum age");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the MD RAID5 optimization mode and validate
  result = parseBool(*argumentPtr++, "on", "off",
                     &config->mdRaid5ModeEnabled);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Invalid MD RAID5 mode");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the write policy and validate.
  if (strcmp(*argumentPtr, "async") == 0) {
    config->writePolicy = WRITE_POLICY_ASYNC;
  } else if (strcmp(*argumentPtr, "sync") == 0) {
    config->writePolicy = WRITE_POLICY_SYNC;
  } else if (strcmp(*argumentPtr, "auto") == 0) {
    config->writePolicy = WRITE_POLICY_AUTO;
  } else {
    handleParseError(&config, errorPtr, "Invalid write policy");
    return VDO_BAD_CONFIGURATION;
  }
  argumentPtr++;

  if (argumentPtr != &argv[POOL_NAME_ARG_INDEX]) {
    handleParseError(&config, errorPtr, "Pool name not in expected location");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the address where the albserver is running. Check for validation
  // is done in dedupe.c code during startKernelLayer call
  result = duplicateString(*argumentPtr++, "pool name", &config->poolName);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Could not copy pool name");
    return VDO_BAD_CONFIGURATION;
  }

  result = duplicateString(*argumentPtr++, "thread config",
                           &config->threadConfigString);
  if (result != VDO_SUCCESS) {
    handleParseError(&config, errorPtr, "Could not copy thread config");
    return VDO_BAD_CONFIGURATION;
  }

  if (strcmp(".", config->threadConfigString) != 0) {
    result = parseThreadConfigString(config->threadConfigString,
                                     &config->threadCounts);
    if (result != VDO_SUCCESS) {
      handleParseError(&config, errorPtr, "Invalid thread-count configuration");
      return VDO_BAD_CONFIGURATION;
    }
  }

  /*
   * Logical, physical, and hash zone counts can all be zero; then we get one
   * thread doing everything, our older configuration. If any zone count is
   * non-zero, the others must be as well.
   */
  if (((config->threadCounts.logicalZones == 0)
       != (config->threadCounts.physicalZones == 0))
      || ((config->threadCounts.physicalZones == 0)
          != (config->threadCounts.hashZones == 0))
      ) {
    handleParseError(&config, errorPtr,
                     "Logical, physical, and hash zones counts must all be"
                     " zero or all non-zero");
    return VDO_BAD_CONFIGURATION;
  }

  *configPtr = config;
  return result;
}

/**********************************************************************/
void freeDeviceConfig(DeviceConfig **configPtr)
{
  if (configPtr == NULL) {
    return;
  }

  DeviceConfig *config = *configPtr;
  if (config == NULL) {
    *configPtr = NULL;
    return;
  }

  FREE(config->threadConfigString);
  FREE(config->poolName);
  FREE(config->parentDeviceName);
  FREE(config->originalString);

  FREE(config);
  *configPtr = NULL;
}

/**********************************************************************/
const char *getConfigWritePolicyString(DeviceConfig *config)
{
  if (config->writePolicy == WRITE_POLICY_AUTO) {
    return "auto";
  }
  return ((config->writePolicy == WRITE_POLICY_ASYNC) ? "async" : "sync");
}

/**********************************************************************/
void resolveConfigWithFlushSupport(DeviceConfig *config, bool flushSupported)
{
  if (config->writePolicy == WRITE_POLICY_AUTO) {
    config->writePolicy
      = (flushSupported ? WRITE_POLICY_ASYNC : WRITE_POLICY_SYNC);
    logInfo("Using write policy %s automatically.",
            getConfigWritePolicyString(config));
  } else {
    logInfo("Using write policy %s.", getConfigWritePolicyString(config));
  }

  if (flushSupported && (config->writePolicy == WRITE_POLICY_SYNC)) {
    logWarning("WARNING: Running in sync mode atop a device supporting flushes"
               " is dangerous!");
  }
}

