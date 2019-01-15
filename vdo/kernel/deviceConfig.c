/**
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/deviceConfig.c#4 $
 */

#include "deviceConfig.h"

#include <linux/device-mapper.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

#include "vdoStringUtils.h"

#include "constants.h"

enum {
  // If we bump this, update the arrays below
  TABLE_VERSION = 2,
  // Limits used when parsing thread-count config spec strings
  BIO_ROTATION_INTERVAL_LIMIT = 1024,
  LOGICAL_THREAD_COUNT_LIMIT  = 60,
  PHYSICAL_THREAD_COUNT_LIMIT = 16,  
  THREAD_COUNT_LIMIT          = 100,
  // XXX The bio-submission queue configuration defaults are temporarily
  // still being defined here until the new runtime-based thread
  // configuration has been fully implemented for managed VDO devices.

  // How many bio submission work queues to use
  DEFAULT_NUM_BIO_SUBMIT_QUEUES             = 4,
  // How often to rotate between bio submission work queues
  DEFAULT_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL  = 64,
};

// arrays for handling different table versions
static const uint8_t REQUIRED_ARGC[] = {10, 12, 9};
static const uint8_t POOL_NAME_ARG_INDEX[] = {8, 10, 8};

/**
 * Decide the version number from argv.
 *
 * @param [in]  argc          The number of table values
 * @param [in]  argv          The array of table values
 * @param [out] error_ptr     A pointer to return a error string in
 * @param [out] version_ptr   A pointer to return the version
 *
 * @return VDO_SUCCESS or an error code
 **/
static int getVersionNumber(int            argc,
                            char         **argv,
                            char         **errorPtr,
                            TableVersion  *version_ptr)
{
  // version, if it exists, is in a form of V<n>
  if (sscanf(argv[0], "V%u", version_ptr) == 1) {
    if (*version_ptr < 1 || *version_ptr > TABLE_VERSION) {
      *errorPtr = "Unknown version number detected";
      return VDO_BAD_CONFIGURATION;
    }
  } else {
    // V0 actually has no version number in the table string
    *version_ptr = 0;
  }

  // V0 and V1 have no optional parameters. There will always be
  // a parameter for thread config, even if its a "." to show
  // its an empty list.
  if (*version_ptr <= 1) {
    if (argc != REQUIRED_ARGC[*version_ptr]) {
      *errorPtr = "Incorrect number of arguments for version";
      return VDO_BAD_CONFIGURATION;      
    }
  } else if (argc < REQUIRED_ARGC[*version_ptr]) {
    *errorPtr = "Incorrect number of arguments for version";
    return VDO_BAD_CONFIGURATION;
  }

  if (*version_ptr != TABLE_VERSION) {
    logWarning("Detected version mismatch between kernel module and tools "
	       " kernel: %d, tool: %d", TABLE_VERSION, *version_ptr);
    logWarning("Please consider upgrading management tools to match kernel.");
  }
  return VDO_SUCCESS; 
}

/**********************************************************************/
int get_pool_name_from_argv(int    argc,
                            char **argv,
                            char **error_ptr,
                            char **pool_name_ptr)
{
  TableVersion version;
  int result = getVersionNumber(argc, argv, error_ptr, &version);
  if (result != VDO_SUCCESS) {
    return result;
  }
  *pool_name_ptr = argv[POOL_NAME_ARG_INDEX[version]];
  return VDO_SUCCESS;
}

/**
 * Resolve the config with write policy, physical size, and other unspecified
 * fields based on the device, if needed.
 *
 * @param [in,out] config   The config possibly missing values
 * @param [in]     verbose  Whether to log about the underlying device
 **/

static void resolveConfigWithDevice(struct device_config *config,
                                    bool                  verbose)
{
  struct dm_dev *dev = config->ownedDevice;
  struct request_queue *requestQueue = bdev_get_queue(dev->bdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,7,0)
  bool flushSupported
    = ((requestQueue->queue_flags & (1ULL << QUEUE_FLAG_WC)) != 0);
  bool fuaSupported
    = ((requestQueue->queue_flags & (1ULL << QUEUE_FLAG_FUA)) != 0);
#else
  bool flushSupported = ((requestQueue->flush_flags & REQ_FLUSH) == REQ_FLUSH);
  bool fuaSupported   = ((requestQueue->flush_flags & REQ_FUA) == REQ_FUA);
#endif
  if (verbose) {
    logInfo("underlying device, REQ_FLUSH: %s, REQ_FUA: %s",
            (flushSupported ? "supported" : "not supported"),
            (fuaSupported ? "supported" : "not supported"));
  } else {
    // We should probably always log, but need to make sure that makes sense
    // before changing behavior.
  }

  if (config->writePolicy == WRITE_POLICY_AUTO) {
    config->writePolicy
      = (flushSupported ? WRITE_POLICY_ASYNC : WRITE_POLICY_SYNC);
    logInfo("Using write policy %s automatically.",
            get_config_write_policy_string(config));
  } else {
    logInfo("Using write policy %s.", get_config_write_policy_string(config));
  }

  if (flushSupported && (config->writePolicy == WRITE_POLICY_SYNC)) {
    logWarning("WARNING: Running in sync mode atop a device supporting flushes"
               " is dangerous!");
  }

  if (config->version == 0) {
    uint64_t device_size = i_size_read(dev->bdev->bd_inode);
    config->physicalBlocks = device_size / VDO_BLOCK_SIZE;
  }
}

/**
 * Parse a two-valued option into a bool.
 *
 * @param [in]  bool_str    The string value to convert to a bool
 * @param [in]  true_str    The string value which should be converted to true
 * @param [in]  false_str   The string value which should be converted to false
 * @param [out] bool_ptr    A pointer to return the bool value in
 *
 * @return VDO_SUCCESS or an error if bool_str is neither true_str
 *                        nor false_str
 **/
__attribute__((warn_unused_result))
static inline int parse_bool(const char *bool_str,
                             const char *true_str,
                             const char *false_str,
                             bool       *bool_ptr)
{
  bool value = false;
  if (strcmp(bool_str, true_str) == 0) {
    value = true;
  } else if (strcmp(bool_str, false_str) == 0) {
    value = false;
  } else {
    return VDO_BAD_CONFIGURATION;
  }

  *bool_ptr = value;
  return VDO_SUCCESS;
}

/**
 * Process one component of a thread parameter configuration string and
 * update the configuration data structure.
 *
 * If the thread count requested is invalid, a message is logged and
 * -EINVAL returned. If the thread name is unknown, a message is logged
 * but no error is returned.
 *
 * @param thread_param_type  The type of thread specified
 * @param count              The thread count requested
 * @param config             The configuration data structure to update
 *
 * @return   VDO_SUCCESS or -EINVAL
 **/
static int process_one_thread_config_spec(const char *thread_param_type,
                                          unsigned int count,
                                          struct thread_count_config *config)
{
  // Handle limited thread parameters
  if (strcmp(thread_param_type, "bioRotationInterval") == 0) {
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
  } else if (strcmp(thread_param_type, "logical") == 0) {
    if (count > LOGICAL_THREAD_COUNT_LIMIT) {
      logError("thread config string error: at most %d 'logical' threads"
               " are allowed",
               LOGICAL_THREAD_COUNT_LIMIT);
      return -EINVAL;
    }
    config->logicalZones = count;
    return VDO_SUCCESS;
  } else if (strcmp(thread_param_type, "physical") == 0) {
    if (count > PHYSICAL_THREAD_COUNT_LIMIT) {
      logError("thread config string error: at most %d 'physical' threads"
               " are allowed",
               PHYSICAL_THREAD_COUNT_LIMIT);
      return -EINVAL;
    }
    config->physicalZones = count;
    return VDO_SUCCESS;
  } else {
    // Handle other thread count parameters
    if (count > THREAD_COUNT_LIMIT) {
      logError("thread config string error: at most %d '%s' threads"
               " are allowed",
               THREAD_COUNT_LIMIT, thread_param_type);
      return -EINVAL;
    }

    if (strcmp(thread_param_type, "hash") == 0) {
      config->hashZones = count;
      return VDO_SUCCESS;
    } else if (strcmp(thread_param_type, "cpu") == 0) {
      if (count == 0) {
        logError("thread config string error:"
                 " at least one 'cpu' thread required");
        return -EINVAL;
      }
      config->cpuThreads = count;
      return VDO_SUCCESS;
    } else if (strcmp(thread_param_type, "ack") == 0) {
      config->bioAckThreads = count;
      return VDO_SUCCESS;
    } else if (strcmp(thread_param_type, "bio") == 0) {
      if (count == 0) {
        logError("thread config string error:"
                 " at least one 'bio' thread required");
        return -EINVAL;
      }
      config->bioThreads = count;
      return VDO_SUCCESS;
    }
  }

  // Don't fail, just log. This will handle version mismatches between
  // user mode tools and kernel.
  logInfo("unknown thread parameter type \"%s\"", thread_param_type);
  return VDO_SUCCESS;
}

/**
 * Parse one component of a thread parameter configuration string and
 * update the configuration data structure.
 *
 * @param spec    The thread parameter specification string
 * @param config  The configuration data to be updated
 **/
static int parse_one_thread_config_spec(const char                 *spec,
                                        struct thread_count_config *config)
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

  result = process_one_thread_config_spec(fields[0], count, config);
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
 * If an error occurs during parsing of a single key/value pair, we deem
 * it serious enough to stop further parsing. 
 *
 * This function can't set the "reason" value the caller wants to pass
 * back, because we'd want to format it to say which field was
 * invalid, and we can't allocate the "reason" strings dynamically. So
 * if an error occurs, we'll log the details and pass back an error.
 *
 * @param string  Thread parameter configuration string
 * @param config  The thread configuration data to update
 *
 * @return   VDO_SUCCESS or -EINVAL or -ENOMEM
 **/
static int parse_thread_config_string(const char                 *string,
                                      struct thread_count_config *config)
{
  int result = VDO_SUCCESS;

  char **specs;
  if (strcmp(".", string) != 0) {
    result = splitString(string, ',', &specs);
    if (result != UDS_SUCCESS) {
      return result;
    }
    for (unsigned int i = 0; specs[i] != NULL; i++) {
      result = parse_one_thread_config_spec(specs[i], config);
      if (result != VDO_SUCCESS) {
	break;
      }
    }
    freeStringArray(specs);
  }
  return result;
}

/**
 * Process one component of an optional parameter string and update
 * the configuration data structure.
 *
 * If the value requested is invalid, a message is logged and -EINVAL
 * returned. If the key is unknown, a message is logged but no error
 * is returned.
 *
 * @param key    The optional parameter key name
 * @param value  The optional parameter value
 * @param config The configuration data structure to update
 *
 * @return   VDO_SUCCESS or -EINVAL
 **/
static int process_one_key_value_pair(const char           *key,
                                      unsigned int          value,
                                      struct device_config *config)
{
  // Non thread optional parameters
  if (strcmp(key, "maxDiscard") == 0) {
    if (value == 0) {
      logError("optional parameter error:"
               " at least one max discard block required");
      return -EINVAL;
    }
    // Max discard sectors in blkdev_issue_discard is UINT_MAX >> 9
    if (value > (UINT_MAX / VDO_BLOCK_SIZE)) {
      logError("optional parameter error: at most %d max discard"
               " blocks are allowed", UINT_MAX / VDO_BLOCK_SIZE);
      return -EINVAL;
    }
    config->maxDiscardBlocks = value;
    return VDO_SUCCESS;
  } 
  // Handles unknown key names
  return process_one_thread_config_spec(key, value, &config->threadCounts);
}

/**
 * Parse one key/value pair and update the configuration 
 * data structure.
 *
 * @param key     The optional key name
 * @param value   The optional value
 * @param config  The configuration data to be updated
 *
 * @return   VDO_SUCCESS or error
 **/
static int parse_one_key_value_pair(const char           *key,
                                    const char           *value,
                                    struct device_config *config)
{
  unsigned int count;
  int result = stringToUInt(value, &count);
  if (result != UDS_SUCCESS) {
    logError("optional config string error: integer value needed, found \"%s\"",
             value);
    return result;
  }
  return process_one_key_value_pair(key, count, config);
}

/**
 * Parse all key/value pairs from a list of arguments.
 *
 * If an error occurs during parsing of a single key/value pair, we deem
 * it serious enough to stop further parsing. 
 *
 * This function can't set the "reason" value the caller wants to pass
 * back, because we'd want to format it to say which field was
 * invalid, and we can't allocate the "reason" strings dynamically. So
 * if an error occurs, we'll log the details and return the error.
 * 
 * @param argc     The total number of arguments in list
 * @param argv     The list of key/value pairs
 * @param config   The device configuration data to update
 *
 * @return   VDO_SUCCESS or error
 **/
static int parse_key_value_pairs(int                    argc, 
                                 char                 **argv, 
                                 struct device_config  *config)
{
  int result = VDO_SUCCESS;
  while (argc) {
    result = parse_one_key_value_pair(argv[0], argv[1], config);
    if (result != VDO_SUCCESS) {
      break;
    }

    argc -= 2;
    argv += 2;
  }

  return result;
}

/**
 * Parse the configuration string passed in for optional arguments.
 * 
 * For V0/V1 configurations, there will only be one optional parameter; 
 * the thread configuration. The configuration string should contain
 * one or more comma-separated specs of the form "typename=number"; the 
 * supported type names are "cpu", "ack", "bio", "bioRotationInterval", 
 * "logical", "physical", and "hash".
 *
 * For V2 configurations and beyond, there could be any number of
 * arguments. They should contain one or more key/value pairs
 * separated by a space.
 *
 * @param argSet   The structure holding the arguments to parse
 * @param error_ptr Pointer to a buffer to hold the error string
 * @param config   Pointer to device configuration data to update
 *
 * @return   VDO_SUCCESS or error
 */
int parse_optional_arguments(struct dm_arg_set     *argSet,
                             char                 **error_ptr,
                             struct device_config  *config)
{
  int result = VDO_SUCCESS;

  if (config->version == 0 || config->version == 1) {
    result = parse_thread_config_string(argSet->argv[0],
				     &config->threadCounts);
    if (result != VDO_SUCCESS) {
      *error_ptr = "Invalid thread-count configuration";
      return VDO_BAD_CONFIGURATION;
    }
  } else {
    if ((argSet->argc % 2) != 0) {
      *error_ptr = "Odd number of optional arguments given but they"
	          " should be <key> <value> pairs";  
      return VDO_BAD_CONFIGURATION;
    }
    result = parse_key_value_pairs(argSet->argc, argSet->argv, config);
    if (result != VDO_SUCCESS) {
      *error_ptr = "Invalid optional argument configuration";
      return VDO_BAD_CONFIGURATION;
    }
  }
  return result;
}

/**
 * Handle a parsing error.
 *
 * @param config_ptr     A pointer to the config to free
 * @param error_ptr      A place to store a constant string about the error
 * @param error_str      A constant string to store in error_ptr
 **/
static void handle_parse_error(struct device_config **config_ptr,
                               char                 **error_ptr,
                               char                  *error_str)
{
  free_device_config(config_ptr);
  *error_ptr = error_str;
}

/**********************************************************************/
int parse_device_config(int                    argc,
                        char                 **argv,
                        struct dm_target      *ti,
                        bool                   verbose,
                        struct device_config **config_ptr)
{
  char **error_ptr = &ti->error;
  struct device_config *config = NULL;
  int result = ALLOCATE(1, struct device_config, "device_config", &config);
  if (result != VDO_SUCCESS) {
    handle_parse_error(&config, error_ptr,
                       "Could not allocate config structure");
    return VDO_BAD_CONFIGURATION;
  }

  config->owningTarget = ti;

  // Save the original string.
  result = joinStrings(argv, argc, ' ', &config->originalString);
  if (result != VDO_SUCCESS) {
    handle_parse_error(&config, error_ptr, "Could not populate string");
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
  config->threadCounts = (struct thread_count_config) {
    .bioAckThreads       = 1,
    .bioThreads          = DEFAULT_NUM_BIO_SUBMIT_QUEUES,
    .bioRotationInterval = DEFAULT_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL,
    .cpuThreads          = 1,
    .logicalZones        = 0,
    .physicalZones       = 0,
    .hashZones           = 0,
  };
  config->maxDiscardBlocks = 1;

  struct dm_arg_set argSet;

  argSet.argc = argc;
  argSet.argv = argv;

  result = getVersionNumber(argc, argv, error_ptr, &config->version);
  if (result != VDO_SUCCESS) {
    // getVersionNumber sets error_ptr itself.
    handle_parse_error(&config, error_ptr, *error_ptr);
    return result;
  }
  // Move the arg pointer forward only if the argument was there.
  if (config->version >= 1) {
    dm_shift_arg(&argSet);
  }

  result = duplicateString(dm_shift_arg(&argSet), "parent device name",
                           &config->parentDeviceName);
  if (result != VDO_SUCCESS) {
    handle_parse_error(&config, error_ptr,
                       "Could not copy parent device name");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the physical blocks, if known.
  if (config->version >= 1) {
    result = kstrtoull(dm_shift_arg(&argSet), 10, &config->physicalBlocks);
    if (result != VDO_SUCCESS) {
      handle_parse_error(&config, error_ptr, "Invalid physical block count");
      return VDO_BAD_CONFIGURATION;
    }
  }

  // Get the logical block size and validate
  bool enable512e;
  result = parse_bool(dm_shift_arg(&argSet), "512", "4096", &enable512e);
  if (result != VDO_SUCCESS) {
    handle_parse_error(&config, error_ptr, "Invalid logical block size");
    return VDO_BAD_CONFIGURATION;
  }
  config->logicalBlockSize = (enable512e ? 512 : 4096);

  // Skip past the two no longer used read cache options.
  if (config->version <= 1) {
    dm_consume_args(&argSet, 2);
  }

  // Get the page cache size.
  result = stringToUInt(dm_shift_arg(&argSet), &config->cacheSize);
  if (result != VDO_SUCCESS) {
    handle_parse_error(&config, error_ptr,
                       "Invalid block map page cache size");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the block map era length.
  result = stringToUInt(dm_shift_arg(&argSet), &config->blockMapMaximumAge);
  if (result != VDO_SUCCESS) {
    handle_parse_error(&config, error_ptr, "Invalid block map maximum age");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the MD RAID5 optimization mode and validate
  result = parse_bool(dm_shift_arg(&argSet), "on", "off",
                      &config->mdRaid5ModeEnabled);
  if (result != VDO_SUCCESS) {
    handle_parse_error(&config, error_ptr, "Invalid MD RAID5 mode");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the write policy and validate.
  if (strcmp(argSet.argv[0], "async") == 0) {
    config->writePolicy = WRITE_POLICY_ASYNC;
  } else if (strcmp(argSet.argv[0], "sync") == 0) {
    config->writePolicy = WRITE_POLICY_SYNC;
  } else if (strcmp(argSet.argv[0], "auto") == 0) {
    config->writePolicy = WRITE_POLICY_AUTO;
  } else {
    handle_parse_error(&config, error_ptr, "Invalid write policy");
    return VDO_BAD_CONFIGURATION;
  }
  dm_shift_arg(&argSet);

  // Make sure the enum to get the pool name from argv directly is still in
  // sync with the parsing of the table line.
  if (&argSet.argv[0] != &argv[POOL_NAME_ARG_INDEX[config->version]]) {
    handle_parse_error(&config, error_ptr,
                       "Pool name not in expected location");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the address where the albserver is running. Check for validation
  // is done in dedupe.c code during startKernelLayer call
  result = duplicateString(dm_shift_arg(&argSet), "pool name", 
			   &config->poolName);
  if (result != VDO_SUCCESS) {
    handle_parse_error(&config, error_ptr, "Could not copy pool name");
    return VDO_BAD_CONFIGURATION;
  }

  // Get the optional arguments and validate.
  result = parse_optional_arguments(&argSet, error_ptr, config);
  if (result != VDO_SUCCESS) {
    // parse_optional_arguments sets error_ptr itself.
    handle_parse_error(&config, error_ptr, *error_ptr);
    return result;
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
    handle_parse_error(&config, error_ptr,
                       "Logical, physical, and hash zones counts must all be"
                       " zero or all non-zero");
    return VDO_BAD_CONFIGURATION;
  }

  result = dm_get_device(ti, config->parentDeviceName,
                         dm_table_get_mode(ti->table), &config->ownedDevice);
  if (result != 0) {
    logError("couldn't open device \"%s\": error %d",
             config->parentDeviceName, result);
    handle_parse_error(&config, error_ptr, "Unable to open storage device");
    return VDO_BAD_CONFIGURATION;
  }

  resolveConfigWithDevice(config, verbose);

  *config_ptr = config;
  return result;
}

/**********************************************************************/
void free_device_config(struct device_config **config_ptr)
{
  if (config_ptr == NULL) {
    return;
  }

  struct device_config *config = *config_ptr;
  if (config == NULL) {
    *config_ptr = NULL;
    return;
  }

  if (config->ownedDevice != NULL) {
    dm_put_device(config->owningTarget, config->ownedDevice);
  }

  FREE(config->poolName);
  FREE(config->parentDeviceName);
  FREE(config->originalString);

  FREE(config);
  *config_ptr = NULL;
}

/**********************************************************************/
const char *get_config_write_policy_string(struct device_config *config)
{
  if (config->writePolicy == WRITE_POLICY_AUTO) {
    return "auto";
  }
  return ((config->writePolicy == WRITE_POLICY_ASYNC) ? "async" : "sync");
}


