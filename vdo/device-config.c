// SPDX-License-Identifier: GPL-2.0-only
/**
 * Copyright Red Hat
 */

#include "device-config.h"

#include <linux/device-mapper.h>

#include "errors.h"
#include "logger.h"
#include "memory-alloc.h"
#include "string-utils.h"

#include "constants.h"
#include "status-codes.h"
#include "types.h"
#include "vdo.h"

enum {
	/* If we bump this, update the arrays below */
	TABLE_VERSION = 4,
};

/* arrays for handling different table versions */
static const uint8_t REQUIRED_ARGC[] = { 10, 12, 9, 7, 6 };
/* pool name no longer used. only here for verification of older versions */
static const uint8_t POOL_NAME_ARG_INDEX[] = { 8, 10, 8 };

/**
 * get_version_number() - Decide the version number from argv.
 *
 * @argc: The number of table values.
 * @argv: The array of table values.
 * @error_ptr: A pointer to return a error string in.
 * @version_ptr: A pointer to return the version.
 *
 * Return: VDO_SUCCESS or an error code.
 **/
static int get_version_number(int argc,
			      char **argv,
			      char **error_ptr,
			      unsigned int *version_ptr)
{
	/* version, if it exists, is in a form of V<n> */
	if (sscanf(argv[0], "V%u", version_ptr) == 1) {
		if (*version_ptr < 1 || *version_ptr > TABLE_VERSION) {
			*error_ptr = "Unknown version number detected";
			return VDO_BAD_CONFIGURATION;
		}
	} else {
		/* V0 actually has no version number in the table string */
		*version_ptr = 0;
	}

	/*
	 * V0 and V1 have no optional parameters. There will always be
	 * a parameter for thread config, even if it's a "." to show
	 * it's an empty list.
	 */
	if (*version_ptr <= 1) {
		if (argc != REQUIRED_ARGC[*version_ptr]) {
			*error_ptr =
				"Incorrect number of arguments for version";
			return VDO_BAD_CONFIGURATION;
		}
	} else if (argc < REQUIRED_ARGC[*version_ptr]) {
		*error_ptr = "Incorrect number of arguments for version";
		return VDO_BAD_CONFIGURATION;
	}

	if (*version_ptr != TABLE_VERSION) {
		uds_log_warning("Detected version mismatch between kernel module and tools kernel: %d, tool: %d",
				TABLE_VERSION,
				*version_ptr);
		uds_log_warning("Please consider upgrading management tools to match kernel.");
	}
	return VDO_SUCCESS;
}

/*
 * Free a list of non-NULL string pointers, and then the list itself.
 */
static void free_string_array(char **string_array)
{
	unsigned int offset;

	for (offset = 0; string_array[offset] != NULL; offset++) {
		UDS_FREE(string_array[offset]);
	}
	UDS_FREE(string_array);
}

/*
 * Split the input string into substrings, separated at occurrences of
 * the indicated character, returning a null-terminated list of string
 * pointers.
 *
 * The string pointers and the pointer array itself should both be
 * freed with UDS_FREE() when no longer needed. This can be done with
 * vdo_free_string_array (below) if the pointers in the array are not
 * changed. Since the array and copied strings are allocated by this
 * function, it may only be used in contexts where allocation is
 * permitted.
 *
 * Empty substrings are not ignored; that is, returned substrings may
 * be empty strings if the separator occurs twice in a row.
 */
static int split_string(const char *string,
			char separator,
			char ***substring_array_ptr)
{
	unsigned int current_substring = 0, substring_count = 1;
	const char *s;
	char **substrings;
	int result;
	ptrdiff_t length;

	for (s = string; *s != 0; s++) {
		if (*s == separator) {
			substring_count++;
		}
	}

	result = UDS_ALLOCATE(substring_count + 1,
			      char *,
			      "string-splitting array",
			      &substrings);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (s = string; *s != 0; s++) {
		if (*s == separator) {
			ptrdiff_t length = s - string;

			result = UDS_ALLOCATE(length + 1,
					      char,
					      "split string",
					      &substrings[current_substring]);
			if (result != UDS_SUCCESS) {
				free_string_array(substrings);
				return result;
			}
			/*
			 * Trailing NUL is already in place after allocation;
			 * deal with the zero or more non-NUL bytes in the
			 * string.
			 */
			if (length > 0) {
				memcpy(substrings[current_substring],
				       string,
				       length);
			}
			string = s + 1;
			current_substring++;
			BUG_ON(current_substring >= substring_count);
		}
	}
	/* Process final string, with no trailing separator. */
	BUG_ON(current_substring != (substring_count - 1));
	length = strlen(string);

	result = UDS_ALLOCATE(length + 1,
			      char,
			      "split string",
			      &substrings[current_substring]);
	if (result != UDS_SUCCESS) {
		free_string_array(substrings);
		return result;
	}
	memcpy(substrings[current_substring], string, length);
	current_substring++;
	/* substrings[current_substring] is NULL already */
	*substring_array_ptr = substrings;
	return UDS_SUCCESS;
}

/*
 * Join the input substrings into one string, joined with the indicated
 * character, returning a string.
 * array_length is a bound on the number of valid elements in
 * substring_array, in case it is not NULL-terminated.
 */
static int join_strings(char **substring_array, size_t array_length,
		        char separator, char **string_ptr)
{
	size_t string_length = 0;
	size_t i;
	int result;
	char *output, *current_position;

	for (i = 0; (i < array_length) && (substring_array[i] != NULL); i++) {
		string_length += strlen(substring_array[i]) + 1;
	}

	result = UDS_ALLOCATE(string_length, char, __func__, &output);

	if (result != VDO_SUCCESS) {
		return result;
	}

	current_position = &output[0];

	for (i = 0; (i < array_length) && (substring_array[i] != NULL); i++) {
		current_position = uds_append_to_buffer(current_position,
							output + string_length,
							"%s",
							substring_array[i]);
		*current_position = separator;
		current_position++;
	}

	/* We output one too many separators; replace the last with a zero byte. */
	if (current_position != output) {
		*(current_position - 1) = '\0';
	}

	*string_ptr = output;
	return UDS_SUCCESS;
}

/*
 * parse_bool() - Parse a two-valued option into a bool.
 * @bool_str: The string value to convert to a bool.
 * @true_str: The string value which should be converted to true.
 * @false_str: The string value which should be converted to false.
 * @bool_ptr: A pointer to return the bool value in.
 *
 * Return: VDO_SUCCESS or an error if bool_str is neither true_str
 *         nor false_str.
 */
static inline int __must_check
parse_bool(const char *bool_str,
	   const char *true_str,
	   const char *false_str,
	   bool *bool_ptr)
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
 * process_one_thread_config_spec() - Process one component of a
 *                                    thread parameter configuration
 *                                    string and update the
 *                                    configuration data structure.
 * @thread_param_type: The type of thread specified.
 * @count: The thread count requested.
 * @config: The configuration data structure to update.
 *
 * If the thread count requested is invalid, a message is logged and
 * -EINVAL returned. If the thread name is unknown, a message is logged
 * but no error is returned.
 *
 * Return: VDO_SUCCESS or -EINVAL
 */
static int process_one_thread_config_spec(const char *thread_param_type,
					  unsigned int count,
					  struct thread_count_config *config)
{
	/* Handle limited thread parameters */
	if (strcmp(thread_param_type, "bioRotationInterval") == 0) {
		if (count == 0) {
			uds_log_error("thread config string error:  'bioRotationInterval' of at least 1 is required");
			return -EINVAL;
		} else if (count > VDO_BIO_ROTATION_INTERVAL_LIMIT) {
			uds_log_error("thread config string error: 'bioRotationInterval' cannot be higher than %d",
				      VDO_BIO_ROTATION_INTERVAL_LIMIT);
			return -EINVAL;
		}
		config->bio_rotation_interval = count;
		return VDO_SUCCESS;
	} else if (strcmp(thread_param_type, "logical") == 0) {
		if (count > MAX_VDO_LOGICAL_ZONES) {
			uds_log_error("thread config string error: at most %d 'logical' threads are allowed",
				      MAX_VDO_LOGICAL_ZONES);
			return -EINVAL;
		}
		config->logical_zones = count;
		return VDO_SUCCESS;
	} else if (strcmp(thread_param_type, "physical") == 0) {
		if (count > MAX_VDO_PHYSICAL_ZONES) {
			uds_log_error("thread config string error: at most %d 'physical' threads are allowed",
				      MAX_VDO_PHYSICAL_ZONES);
			return -EINVAL;
		}
		config->physical_zones = count;
		return VDO_SUCCESS;
	} else {
		/* Handle other thread count parameters */
		if (count > MAXIMUM_VDO_THREADS) {
			uds_log_error("thread config string error: at most %d '%s' threads are allowed",
				      MAXIMUM_VDO_THREADS,
				      thread_param_type);
			return -EINVAL;
		}

		if (strcmp(thread_param_type, "hash") == 0) {
			config->hash_zones = count;
			return VDO_SUCCESS;
		} else if (strcmp(thread_param_type, "cpu") == 0) {
			if (count == 0) {
				uds_log_error("thread config string error: at least one 'cpu' thread required");
				return -EINVAL;
			}
			config->cpu_threads = count;
			return VDO_SUCCESS;
		} else if (strcmp(thread_param_type, "ack") == 0) {
			config->bio_ack_threads = count;
			return VDO_SUCCESS;
		} else if (strcmp(thread_param_type, "bio") == 0) {
			if (count == 0) {
				uds_log_error("thread config string error: at least one 'bio' thread required");
				return -EINVAL;
			}
			config->bio_threads = count;
			return VDO_SUCCESS;
		}
	}

	/*
	 * Don't fail, just log. This will handle version mismatches between
	 * user mode tools and kernel.
	 */
	uds_log_info("unknown thread parameter type \"%s\"",
		     thread_param_type);
	return VDO_SUCCESS;
}

/**
 * parse_one_thread_config_spec() - Parse one component of a thread
 *                                  parameter configuration string and
 *                                  update the configuration data
 *                                  structure.
 * @spec: The thread parameter specification string.
 * @config: The configuration data to be updated.
 */
static int parse_one_thread_config_spec(const char *spec,
					struct thread_count_config *config)
{
	unsigned int count;
	char **fields;
	int result = split_string(spec, '=', &fields);

	if (result != UDS_SUCCESS) {
		return result;
	}
	if ((fields[0] == NULL) || (fields[1] == NULL) || (fields[2] != NULL)) {
		uds_log_error("thread config string error: expected thread parameter assignment, saw \"%s\"",
			      spec);
		free_string_array(fields);
		return -EINVAL;
	}

	result = kstrtouint(fields[1], 10, &count);
	if (result != UDS_SUCCESS) {
		uds_log_error("thread config string error: integer value needed, found \"%s\"",
			      fields[1]);
		free_string_array(fields);
		return result;
	}

	result = process_one_thread_config_spec(fields[0], count, config);
	free_string_array(fields);
	return result;
}

/**
 * parse_thread_config_string() - Parse the configuration string
 *                                passed and update the specified
 *                                counts and other parameters of
 *                                various types of threads to be
 *                                created.
 * @string: Thread parameter configuration string.
 * @config: The thread configuration data to update.
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
 * Return: VDO_SUCCESS or -EINVAL or -ENOMEM
 */
static int parse_thread_config_string(const char *string,
				      struct thread_count_config *config)
{
	int result = VDO_SUCCESS;

	char **specs;

	if (strcmp(".", string) != 0) {
		unsigned int i;

		result = split_string(string, ',', &specs);
		if (result != UDS_SUCCESS) {
			return result;
		}

		for (i = 0; specs[i] != NULL; i++) {
			result = parse_one_thread_config_spec(specs[i], config);
			if (result != VDO_SUCCESS) {
				break;
			}
		}
		free_string_array(specs);
	}
	return result;
}

/**
 * process_one_key_value_pair() - Process one component of an optional
 *                                parameter string and update the
 *                                configuration data structure.
 * @key: The optional parameter key name.
 * @value: The optional parameter value.
 * @config: The configuration data structure to update.
 *
 * If the value requested is invalid, a message is logged and -EINVAL
 * returned. If the key is unknown, a message is logged but no error
 * is returned.
 *
 * Return: VDO_SUCCESS or -EINVAL
 */
static int process_one_key_value_pair(const char *key,
				      unsigned int value,
				      struct device_config *config)
{
	/* Non thread optional parameters */
	if (strcmp(key, "maxDiscard") == 0) {
		if (value == 0) {
			uds_log_error("optional parameter error: at least one max discard block required");
			return -EINVAL;
		}
		/* Max discard sectors in blkdev_issue_discard is UINT_MAX >> 9 */
		if (value > (UINT_MAX / VDO_BLOCK_SIZE)) {
			uds_log_error("optional parameter error: at most %d max discard  blocks are allowed",
				      UINT_MAX / VDO_BLOCK_SIZE);
			return -EINVAL;
		}
		config->max_discard_blocks = value;
		return VDO_SUCCESS;
	}
	/* Handles unknown key names */
	return process_one_thread_config_spec(key, value,
					      &config->thread_counts);
}

/**
 * parse_one_key_value_pair() - Parse one key/value pair and update
 *                              the configuration data structure.
 * @key: The optional key name.
 * @value: The optional value.
 * @config: The configuration data to be updated.
 *
 * Return: VDO_SUCCESS or error.
 */
static int parse_one_key_value_pair(const char *key,
				    const char *value,
				    struct device_config *config)
{
	unsigned int count;
	int result;

	if (strcmp(key, "deduplication") == 0) {
		return parse_bool(value, "on", "off", &config->deduplication);
	}

	if (strcmp(key, "compression") == 0) {
		return parse_bool(value, "on", "off", &config->compression);
	}

	/* The remaining arguments must have integral values. */
	result = kstrtouint(value, 10, &count);
	if (result != UDS_SUCCESS) {
		uds_log_error("optional config string error: integer value needed, found \"%s\"",
			      value);
		return result;
	}
	return process_one_key_value_pair(key, count, config);
}

/**
 * parse_key_value_pairs() - Parse all key/value pairs from a list of
 *                           arguments.
 * @argc: The total number of arguments in list.
 * @argv: The list of key/value pairs.
 * @config: The device configuration data to update.
 *
 * If an error occurs during parsing of a single key/value pair, we deem
 * it serious enough to stop further parsing.
 *
 * This function can't set the "reason" value the caller wants to pass
 * back, because we'd want to format it to say which field was
 * invalid, and we can't allocate the "reason" strings dynamically. So
 * if an error occurs, we'll log the details and return the error.
 *
 * Return: VDO_SUCCESS or error
 */
static int parse_key_value_pairs(int argc,
				 char **argv,
				 struct device_config *config)
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
 * parse_optional_arguments() - Parse the configuration string passed
 *                              in for optional arguments.
 * @arg_set: The structure holding the arguments to parse.
 * @error_ptr: Pointer to a buffer to hold the error string.
 * @config: Pointer to device configuration data to update.
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
 * Return: VDO_SUCCESS or error
 */
int parse_optional_arguments(struct dm_arg_set *arg_set,
			     char **error_ptr,
			     struct device_config *config)
{
	int result = VDO_SUCCESS;

	if (config->version == 0 || config->version == 1) {
		result = parse_thread_config_string(arg_set->argv[0],
						    &config->thread_counts);
		if (result != VDO_SUCCESS) {
			*error_ptr = "Invalid thread-count configuration";
			return VDO_BAD_CONFIGURATION;
		}
	} else {
		if ((arg_set->argc % 2) != 0) {
			*error_ptr = "Odd number of optional arguments given but they should be <key> <value> pairs";
			return VDO_BAD_CONFIGURATION;
		}
		result = parse_key_value_pairs(arg_set->argc,
					       arg_set->argv,
					       config);
		if (result != VDO_SUCCESS) {
			*error_ptr = "Invalid optional argument configuration";
			return VDO_BAD_CONFIGURATION;
		}
	}
	return result;
}

/**
 * handle_parse_error() - Handle a parsing error.
 * @config: The config to free.
 * @error_ptr: A place to store a constant string about the error.
 * @error_str: A constant string to store in error_ptr.
 */
static void handle_parse_error(struct device_config *config,
			       char **error_ptr,
			       char *error_str)
{
	vdo_free_device_config(config);
	*error_ptr = error_str;
}

/**
 * vdo_parse_device_config() - Convert the dmsetup table into a struct
 *                             device_config.
 * @argc: The number of table values.
 * @argv: The array of table values.
 * @ti: The target structure for this table.
 * @config_ptr: A pointer to return the allocated config.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_parse_device_config(int argc,
			    char **argv,
			    struct dm_target *ti,
			    struct device_config **config_ptr)
{
	bool enable_512e;
	size_t logical_bytes = to_bytes(ti->len);
	struct dm_arg_set arg_set;
	char **error_ptr = &ti->error;
	struct device_config *config = NULL;
	int result;


	if ((logical_bytes % VDO_BLOCK_SIZE) != 0) {
		handle_parse_error(config,
				   error_ptr,
				   "Logical size must be a multiple of 4096");
		return VDO_BAD_CONFIGURATION;
	}

	if (argc == 0) {
		handle_parse_error(config,
				   error_ptr,
				   "Incorrect number of arguments");
		return VDO_BAD_CONFIGURATION;
	}

	result = UDS_ALLOCATE(1,
			      struct device_config,
			      "device_config",
			      &config);
	if (result != VDO_SUCCESS) {
		handle_parse_error(config,
				   error_ptr,
				   "Could not allocate config structure");
		return VDO_BAD_CONFIGURATION;
	}

	config->owning_target = ti;
	config->logical_blocks = logical_bytes / VDO_BLOCK_SIZE;
	INIT_LIST_HEAD(&config->config_list);

	/* Save the original string. */
	result = join_strings(argv, argc, ' ', &config->original_string);
	if (result != VDO_SUCCESS) {
		handle_parse_error(config,
				   error_ptr,
				   "Could not populate string");
		return VDO_BAD_CONFIGURATION;
	}

	/* Set defaults.
	 *
	 * XXX Defaults for bio_threads and bio_rotation_interval are currently
	 * defined using the old configuration scheme of constants.  These
	 * values are relied upon for performance testing on MGH machines
	 * currently. This should be replaced with the normally used testing
	 * defaults being defined in the file-based thread-configuration
	 * settings.  The values used as defaults internally should really be
	 * those needed for VDO in its default shipped-product state.
	 */
	config->thread_counts = (struct thread_count_config) {
		.bio_ack_threads = 1,
		.bio_threads = DEFAULT_VDO_BIO_SUBMIT_QUEUE_COUNT,
		.bio_rotation_interval =
			DEFAULT_VDO_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL,
		.cpu_threads = 1,
		.logical_zones = 0,
		.physical_zones = 0,
		.hash_zones = 0,
	};
	config->max_discard_blocks = 1;
	config->deduplication = true;
	config->compression = false;

	arg_set.argc = argc;
	arg_set.argv = argv;

	result = get_version_number(argc, argv, error_ptr, &config->version);
	if (result != VDO_SUCCESS) {
		/* get_version_number sets error_ptr itself. */
		handle_parse_error(config, error_ptr, *error_ptr);
		return result;
	}
	/* Move the arg pointer forward only if the argument was there. */
	if (config->version >= 1) {
		dm_shift_arg(&arg_set);
	}

	result = uds_duplicate_string(dm_shift_arg(&arg_set),
				      "parent device name",
				      &config->parent_device_name);
	if (result != VDO_SUCCESS) {
		handle_parse_error(config,
				   error_ptr,
				   "Could not copy parent device name");
		return VDO_BAD_CONFIGURATION;
	}

	/* Get the physical blocks, if known. */
	if (config->version >= 1) {
		result = kstrtoull(dm_shift_arg(&arg_set),
				   10,
				   &config->physical_blocks);
		if (result != VDO_SUCCESS) {
			handle_parse_error(config,
					   error_ptr,
					   "Invalid physical block count");
			return VDO_BAD_CONFIGURATION;
		}
	}

	/* Get the logical block size and validate */
	result = parse_bool(dm_shift_arg(&arg_set),
			    "512",
			    "4096",
			    &enable_512e);
	if (result != VDO_SUCCESS) {
		handle_parse_error(config,
				   error_ptr,
				   "Invalid logical block size");
		return VDO_BAD_CONFIGURATION;
	}
	config->logical_block_size = (enable_512e ? 512 : 4096);

	/* Skip past the two no longer used read cache options. */
	if (config->version <= 1) {
		dm_consume_args(&arg_set, 2);
	}

	/* Get the page cache size. */
	result = kstrtouint(dm_shift_arg(&arg_set), 10, &config->cache_size);
	if (result != VDO_SUCCESS) {
		handle_parse_error(config,
				   error_ptr,
				   "Invalid block map page cache size");
		return VDO_BAD_CONFIGURATION;
	}

	/* Get the block map era length. */
	result = kstrtouint(dm_shift_arg(&arg_set), 10,
			    &config->block_map_maximum_age);
	if (result != VDO_SUCCESS) {
		handle_parse_error(config,
				   error_ptr,
				   "Invalid block map maximum age");
		return VDO_BAD_CONFIGURATION;
	}

	/* Skip past the no longer used MD RAID5 optimization mode */
	if (config->version <= 2) {
		dm_consume_args(&arg_set, 1);
	}

	/* Skip past the no longer used write policy setting */
	if (config->version <= 3) {
		dm_consume_args(&arg_set, 1);
	}

	/* Skip past the no longer used pool name for older table lines */
	if (config->version <= 2) {
		/*
		 * Make sure the enum to get the pool name from argv directly
		 * is still in sync with the parsing of the table line.
		 */
		if (&arg_set.argv[0] !=
		    &argv[POOL_NAME_ARG_INDEX[config->version]]) {
			handle_parse_error(config,
					   error_ptr,
					   "Pool name not in expected location");
			return VDO_BAD_CONFIGURATION;
		}
		dm_shift_arg(&arg_set);
	}

	/* Get the optional arguments and validate. */
	result = parse_optional_arguments(&arg_set, error_ptr, config);
	if (result != VDO_SUCCESS) {
		/* parse_optional_arguments sets error_ptr itself. */
		handle_parse_error(config, error_ptr, *error_ptr);
		return result;
	}

	/*
	 * Logical, physical, and hash zone counts can all be zero; then we get
	 * one thread doing everything, our older configuration. If any zone
	 * count is non-zero, the others must be as well.
	 */
	if (((config->thread_counts.logical_zones == 0) !=
	     (config->thread_counts.physical_zones == 0)) ||
	    ((config->thread_counts.physical_zones == 0) !=
	     (config->thread_counts.hash_zones == 0))) {
		handle_parse_error(config,
				   error_ptr,
				   "Logical, physical, and hash zones counts must all be zero or all non-zero");
		return VDO_BAD_CONFIGURATION;
	}

	if (config->cache_size <
	    (2 * MAXIMUM_VDO_USER_VIOS * config->thread_counts.logical_zones)) {
		handle_parse_error(config,
				   error_ptr,
				   "Insufficient block map cache for logical zones");
		return VDO_BAD_CONFIGURATION;
	}

	result = dm_get_device(ti,
			       config->parent_device_name,
			       dm_table_get_mode(ti->table),
			       &config->owned_device);
	if (result != 0) {
		uds_log_error("couldn't open device \"%s\": error %d",
			      config->parent_device_name,
			      result);
		handle_parse_error(config,
				   error_ptr,
				   "Unable to open storage device");
		return VDO_BAD_CONFIGURATION;
	}

	if (config->version == 0) {
		uint64_t device_size =
			i_size_read(config->owned_device->bdev->bd_inode);

		config->physical_blocks = device_size / VDO_BLOCK_SIZE;
	}

	*config_ptr = config;
	return result;
}

/**
 * vdo_free_device_config() - Free a device config created by
 *                            vdo_parse_device_config().
 * @config: The config to free.
 */
void vdo_free_device_config(struct device_config *config)
{
	if (config == NULL) {
		return;
	}

	if (config->owned_device != NULL) {
		dm_put_device(config->owning_target, config->owned_device);
	}

	UDS_FREE(config->parent_device_name);
	UDS_FREE(config->original_string);

	/*
         * Reduce the chance a use-after-free (as in BZ 1669960) happens to
	 * work.
	 */
	memset(config, 0, sizeof(*config));
	UDS_FREE(config);
}

/**
 * vdo_set_device_config() - Acquire or release a reference from the
 *                           config to a vdo.
 * @config: The config in question.
 * @vdo: The vdo in question.
 */
void vdo_set_device_config(struct device_config *config, struct vdo *vdo)
{
	list_del_init(&config->config_list);
	if (vdo != NULL) {
		list_add_tail(&config->config_list, &vdo->device_config_list);

	}

	config->vdo = vdo;
}

/**
 * vdo_validate_new_device_config() - Check whether a new device
 *                                    config represents a valid
 *                                    modification to an existing
 *                                    config.
 * @to_validate: The new config to valudate.
 * @config: The existing config.
 * @may_grow: Set to true if growing the logical and physical size of
 *            the vdo is currently permitted.
 * @error_ptr: A pointer to hold the reason for any error.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_validate_new_device_config(struct device_config *to_validate,
				   struct device_config *config,
				   bool may_grow,
				   char **error_ptr)
{
	if (to_validate->owning_target->begin !=
	    config->owning_target->begin) {
		*error_ptr = "Starting sector cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (to_validate->logical_block_size != config->logical_block_size) {
		*error_ptr = "Logical block size cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (to_validate->logical_blocks < config->logical_blocks) {
		*error_ptr = "Can't shrink VDO logical size";
		return VDO_PARAMETER_MISMATCH;
	}

	if (!may_grow
	    && (to_validate->logical_blocks > config->logical_blocks)) {
		*error_ptr = "VDO logical size may not grow in current state";
		return VDO_NOT_IMPLEMENTED;
	}

	if (to_validate->cache_size != config->cache_size) {
		*error_ptr = "Block map cache size cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (to_validate->block_map_maximum_age !=
	    config->block_map_maximum_age) {
		*error_ptr = "Block map maximum age cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (memcmp(&config->thread_counts, &config->thread_counts,
		   sizeof(struct thread_count_config)) != 0) {
		*error_ptr = "Thread configuration cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (to_validate->physical_blocks < config->physical_blocks) {
		*error_ptr = "Removing physical storage from a VDO is not supported";
		return VDO_NOT_IMPLEMENTED;
	}

	if (!may_grow
	    && (to_validate->physical_blocks > config->physical_blocks)) {
		*error_ptr = "VDO physical size may not grow in current state";
		return VDO_NOT_IMPLEMENTED;
	}

	return VDO_SUCCESS;
}
