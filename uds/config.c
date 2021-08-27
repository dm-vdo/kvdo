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
 * $Id: //eng/uds-releases/lisa/src/uds/config.c#2 $
 */

#include "config.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

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
	result = skip_forward(buffer, sizeof(uint32_t));
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
	result = skip_forward(buffer, sizeof(uint32_t));
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
	result = zero_bytes(buffer, sizeof(uint32_t));
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
	result = zero_bytes(buffer, sizeof(uint32_t));
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

/**********************************************************************/
bool are_uds_configurations_equal(struct uds_configuration *a,
				  struct uds_configuration *b)
{
	bool result = true;
	if (a->record_pages_per_chapter != b->record_pages_per_chapter) {
		uds_log_error("Record pages per chapter (%u) does not match (%u)",
			      a->record_pages_per_chapter,
			      b->record_pages_per_chapter);
		result = false;
	}
	if (a->chapters_per_volume != b->chapters_per_volume) {
		uds_log_error("Chapter count (%u) does not match (%u)",
			      a->chapters_per_volume,
			      b->chapters_per_volume);
		result = false;
	}
	if (a->sparse_chapters_per_volume != b->sparse_chapters_per_volume) {
		uds_log_error("Sparse chapter count (%u) does not match (%u)",
			      a->sparse_chapters_per_volume,
			      b->sparse_chapters_per_volume);
		result = false;
	}
	if (a->cache_chapters != b->cache_chapters) {
		uds_log_error("Cache size (%u) does not match (%u)",
			      a->cache_chapters,
			      b->cache_chapters);
		result = false;
	}
	if (a->volume_index_mean_delta != b->volume_index_mean_delta) {
		uds_log_error("Volumee index mean delta (%u) does not match (%u)",
			      a->volume_index_mean_delta,
			      b->volume_index_mean_delta);
		result = false;
	}
	if (a->bytes_per_page != b->bytes_per_page) {
		uds_log_error("Bytes per page value (%u) does not match (%u)",
			      a->bytes_per_page,
			      b->bytes_per_page);
		result = false;
	}
	if (a->sparse_sample_rate != b->sparse_sample_rate) {
		uds_log_error("Sparse sample rate (%u) does not match (%u)",
			      a->sparse_sample_rate,
			      b->sparse_sample_rate);
		result = false;
	}
	if (a->nonce != b->nonce) {
		uds_log_error("Nonce (%llu) does not match (%llu)",
			      (unsigned long long) a->nonce,
			      (unsigned long long) b->nonce);
		result = false;
	}
	return result;
}

/**********************************************************************/
void log_uds_configuration(struct uds_configuration *conf)
{
	uds_log_debug("Configuration:");
	uds_log_debug("  Record pages per chapter:   %10u",
		      conf->record_pages_per_chapter);
	uds_log_debug("  Chapters per volume:        %10u",
		      conf->chapters_per_volume);
	uds_log_debug("  Sparse chapters per volume: %10u",
		      conf->sparse_chapters_per_volume);
	uds_log_debug("  Cache size (chapters):      %10u",
		      conf->cache_chapters);
	uds_log_debug("  Volume index mean delta:    %10u",
		      conf->volume_index_mean_delta);
	uds_log_debug("  Bytes per page:             %10u",
		      conf->bytes_per_page);
	uds_log_debug("  Sparse sample rate:         %10u",
		      conf->sparse_sample_rate);
	uds_log_debug("  Nonce:                      %llu",
		      (unsigned long long) conf->nonce);
}
