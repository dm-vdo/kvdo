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
 * $Id: //eng/uds-releases/krusty-rhel9.0-beta/src/uds/indexConfig.c#1 $
 */

#include "indexConfig.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"

static const byte INDEX_CONFIG_MAGIC[] = "ALBIC";
static const byte INDEX_CONFIG_VERSION_6_02[] = "06.02";
static const byte INDEX_CONFIG_VERSION_8_02[] = "08.02";

enum {
	INDEX_CONFIG_MAGIC_LENGTH = sizeof(INDEX_CONFIG_MAGIC) - 1,
	INDEX_CONFIG_VERSION_LENGTH = sizeof(INDEX_CONFIG_VERSION_6_02) - 1
};

/**********************************************************************/
static int __must_check
decode_index_config_06_02(struct buffer *buffer,
			  struct uds_configuration *config)
{
	int result =
		get_uint32_le_from_buffer(buffer,
					  &config->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		get_uint32_le_from_buffer(buffer,
					  &config->chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		get_uint32_le_from_buffer(buffer,
					  &config->sparse_chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &config->cache_chapters);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer,
					   &config->checkpoint_frequency);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer,
					   &config->volume_index_mean_delta);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &config->bytes_per_page);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		get_uint32_le_from_buffer(buffer, &config->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &config->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->remapped_virtual = 0;
	config->remapped_physical = 0;
	if (ASSERT_LOG_ONLY(content_length(buffer) == 0,
			    "%zu bytes decoded of %zu expected",
			    buffer_length(buffer) - content_length(buffer),
			    buffer_length(buffer)) != UDS_SUCCESS) {
		return UDS_CORRUPT_COMPONENT;
	}
	return result;
}

/**********************************************************************/
static int __must_check
decode_index_config_08_02(struct buffer *buffer,
			  struct uds_configuration *config)
{
	int result =
		get_uint32_le_from_buffer(buffer,
					  &config->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		get_uint32_le_from_buffer(buffer,
					  &config->chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		get_uint32_le_from_buffer(buffer,
					  &config->sparse_chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &config->cache_chapters);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer,
					   &config->checkpoint_frequency);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer,
					   &config->volume_index_mean_delta);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &config->bytes_per_page);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		get_uint32_le_from_buffer(buffer, &config->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &config->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &config->remapped_virtual);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &config->remapped_physical);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if (ASSERT_LOG_ONLY(content_length(buffer) == 0,
			    "%zu bytes decoded of %zu expected",
			    buffer_length(buffer) - content_length(buffer),
			    buffer_length(buffer)) != UDS_SUCCESS) {
		return UDS_CORRUPT_COMPONENT;
	}
	return result;
}

/**********************************************************************/
static int read_version(struct buffered_reader *reader,
			struct uds_configuration *conf)
{
	byte version_buffer[INDEX_CONFIG_VERSION_LENGTH];
	int result = read_from_buffered_reader(reader, version_buffer,
					       INDEX_CONFIG_VERSION_LENGTH);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot read index config version");
	}
	if (memcmp(INDEX_CONFIG_VERSION_6_02, version_buffer,
		   INDEX_CONFIG_VERSION_LENGTH) == 0) {
		struct buffer *buffer;
		result = make_buffer(sizeof(struct uds_configuration_6_02),
				     &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = read_from_buffered_reader(reader,
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return uds_log_error_strerror(result,
						      "cannot read config data");
		}

		clear_buffer(buffer);
		result = decode_index_config_06_02(buffer, conf);
		free_buffer(UDS_FORGET(buffer));
	} else if (memcmp(INDEX_CONFIG_VERSION_8_02, version_buffer,
			  INDEX_CONFIG_VERSION_LENGTH) == 0) {
		struct buffer *buffer;
		result = make_buffer(sizeof(struct uds_configuration), &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = read_from_buffered_reader(reader,
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return uds_log_error_strerror(result,
						      "cannot read config data");
		}
		clear_buffer(buffer);
		result = decode_index_config_08_02(buffer, conf);
		free_buffer(UDS_FORGET(buffer));
	} else {
		uds_log_error_strerror(result,
				       "unsupported configuration version: '%.*s'",
				       INDEX_CONFIG_VERSION_LENGTH,
				       version_buffer);
		result = UDS_CORRUPT_COMPONENT;
	}
	return result;
}

/**********************************************************************/
int read_config_contents(struct buffered_reader *reader,
			 struct uds_configuration *config)
{
	int result = verify_buffered_data(reader, INDEX_CONFIG_MAGIC,
					  INDEX_CONFIG_MAGIC_LENGTH);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_version(reader, config);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "Failed to read index config");
	}
	return result;
}

/**********************************************************************/
static int __must_check
encode_index_config_06_02(struct buffer *buffer,
			  struct uds_configuration *config)
{
	int result =
		put_uint32_le_into_buffer(buffer,
					  config->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		put_uint32_le_into_buffer(buffer, config->chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   config->sparse_chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, config->cache_chapters);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		put_uint32_le_into_buffer(buffer,
					  config->checkpoint_frequency);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   config->volume_index_mean_delta);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, config->bytes_per_page);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, config->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, config->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return ASSERT_LOG_ONLY(content_length(buffer) ==
			       sizeof(struct uds_configuration_6_02),
			       "%zu bytes encoded, of %zu expected",
			       content_length(buffer),
			       sizeof(*config));
}

/**********************************************************************/
static int __must_check
encode_index_config_08_02(struct buffer *buffer,
			  struct uds_configuration *config)
{
	int result =
		put_uint32_le_into_buffer(buffer,
					  config->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		put_uint32_le_into_buffer(buffer, config->chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   config->sparse_chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, config->cache_chapters);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		put_uint32_le_into_buffer(buffer,
					  config->checkpoint_frequency);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   config->volume_index_mean_delta);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, config->bytes_per_page);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, config->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, config->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, config->remapped_virtual);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer,
					   config->remapped_physical);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return ASSERT_LOG_ONLY(content_length(buffer) ==
			       sizeof(struct uds_configuration),
			       "%zu bytes encoded, of %zu expected",
			       content_length(buffer),
			       sizeof(*config));
}

/**********************************************************************/
int write_config_contents(struct buffered_writer *writer,
			  struct uds_configuration *config,
			  uint32_t version)
{
	struct buffer *buffer;
	int result = write_to_buffered_writer(writer, INDEX_CONFIG_MAGIC,
					      INDEX_CONFIG_MAGIC_LENGTH);
	if (result != UDS_SUCCESS) {
		return result;
	}
	/*
	 * If version is < 4, the index has not been reduced by a
	 * chapter so it must be written out as version 6.02 so that
	 * it is still compatible with older versions of UDS.
	 */
	if (version < 4) {
		result = write_to_buffered_writer(writer,
						  INDEX_CONFIG_VERSION_6_02,
						  INDEX_CONFIG_VERSION_LENGTH);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = make_buffer(sizeof(struct uds_configuration_6_02),
				     &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = encode_index_config_06_02(buffer, config);
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}
	} else {
		result = write_to_buffered_writer(writer,
						  INDEX_CONFIG_VERSION_8_02,
						  INDEX_CONFIG_VERSION_LENGTH);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = make_buffer(sizeof(struct uds_configuration), &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = encode_index_config_08_02(buffer, config);
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}
	}
	result = write_to_buffered_writer(writer, get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	return result;
}

/**********************************************************************/
int make_configuration(const struct uds_configuration *conf,
		       struct configuration **config_ptr)
{
	struct configuration *config;
	int result = UDS_ALLOCATE(1, struct configuration, "configuration",
				  &config);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = make_geometry(conf->bytes_per_page,
			       conf->record_pages_per_chapter,
			       conf->chapters_per_volume,
			       conf->sparse_chapters_per_volume,
			       conf->remapped_virtual,
			       conf->remapped_physical,
			       &config->geometry);
	if (result != UDS_SUCCESS) {
		free_configuration(config);
		return result;
	}

	config->sparse_sample_rate = conf->sparse_sample_rate;
	config->cache_chapters = conf->cache_chapters;
	config->volume_index_mean_delta = conf->volume_index_mean_delta;

	*config_ptr = config;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_configuration(struct configuration *config)
{
	if (config != NULL) {
		free_geometry(config->geometry);
		UDS_FREE(config);
	}
}
