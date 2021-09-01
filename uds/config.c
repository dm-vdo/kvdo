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
 * $Id: //eng/uds-releases/lisa/src/uds/config.c#3 $
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
			  struct uds_configuration_8_02 *config)
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
			    "%zu bytes read but not decoded",
			    content_length(buffer)) != UDS_SUCCESS) {
		return UDS_CORRUPT_COMPONENT;
	}
	return result;
}

/**********************************************************************/
static int __must_check
decode_index_config_08_02(struct buffer *buffer,
			  struct uds_configuration_8_02 *config)
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
			    "%zu bytes read but not decoded",
			    content_length(buffer)) != UDS_SUCCESS) {
		return UDS_CORRUPT_COMPONENT;
	}
	return result;
}

/**********************************************************************/
static int read_version(struct buffered_reader *reader,
			struct uds_configuration_8_02 *conf)
{
	byte version_buffer[INDEX_CONFIG_VERSION_LENGTH];
	struct buffer *buffer;
	int result;

	result = read_from_buffered_reader(reader,
					   version_buffer,
					   INDEX_CONFIG_VERSION_LENGTH);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot read index config version");
	}
	if (memcmp(INDEX_CONFIG_VERSION_6_02, version_buffer,
		   INDEX_CONFIG_VERSION_LENGTH) == 0) {
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
		result = make_buffer(sizeof(struct uds_configuration_8_02),
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
static bool are_matching_configurations(struct uds_configuration_8_02 *saved,
					struct configuration *user)
{
        struct geometry *geometry = user->geometry;
	bool result = true;

	if (saved->record_pages_per_chapter !=
	    geometry->record_pages_per_chapter) {
		uds_log_error("Record pages per chapter (%u) does not match (%u)",
			      saved->record_pages_per_chapter,
			      geometry->record_pages_per_chapter);
		result = false;
	}
	if (saved->chapters_per_volume != geometry->chapters_per_volume) {
		uds_log_error("Chapter count (%u) does not match (%u)",
			      saved->chapters_per_volume,
			      geometry->chapters_per_volume);
		result = false;
	}
	if (saved->sparse_chapters_per_volume !=
	    geometry->sparse_chapters_per_volume) {
		uds_log_error("Sparse chapter count (%u) does not match (%u)",
			      saved->sparse_chapters_per_volume,
			      geometry->sparse_chapters_per_volume);
		result = false;
	}
	if (saved->cache_chapters != user->cache_chapters) {
		uds_log_error("Cache size (%u) does not match (%u)",
			      saved->cache_chapters,
			      user->cache_chapters);
		result = false;
	}
	if (saved->volume_index_mean_delta != user->volume_index_mean_delta) {
		uds_log_error("Volumee index mean delta (%u) does not match (%u)",
			      saved->volume_index_mean_delta,
			      user->volume_index_mean_delta);
		result = false;
	}
	if (saved->bytes_per_page != geometry->bytes_per_page) {
		uds_log_error("Bytes per page value (%u) does not match (%zu)",
			      saved->bytes_per_page,
			      geometry->bytes_per_page);
		result = false;
	}
	if (saved->sparse_sample_rate != user->sparse_sample_rate) {
		uds_log_error("Sparse sample rate (%u) does not match (%u)",
			      saved->sparse_sample_rate,
			      user->sparse_sample_rate);
		result = false;
	}
	if (saved->nonce != user->nonce) {
		uds_log_error("Nonce (%llu) does not match (%llu)",
			      (unsigned long long) saved->nonce,
			      (unsigned long long) user->nonce);
		result = false;
	}
	return result;
}

/**********************************************************************/
int validate_config_contents(struct buffered_reader *reader,
			     struct configuration *config)
{
	struct uds_configuration_8_02 saved;
	int result = verify_buffered_data(reader, INDEX_CONFIG_MAGIC,
					  INDEX_CONFIG_MAGIC_LENGTH);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_version(reader, &saved);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "Failed to read index config");
		return result;
	}

	if (!are_matching_configurations(&saved, config)) {
		uds_log_warning("Supplied configuration does not match save");
		return UDS_NO_INDEX;
	}

	config->geometry->remapped_virtual = saved.remapped_virtual;
	config->geometry->remapped_physical = saved.remapped_physical;
	return UDS_SUCCESS;
}

/**********************************************************************/
static int __must_check
encode_index_config_06_02(struct buffer *buffer, struct configuration *config)
{
	int result;
        struct geometry *geometry = config->geometry;

	result = put_uint32_le_into_buffer(buffer,
					   geometry->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   geometry->chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   geometry->sparse_chapters_per_volume);
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
	result = put_uint32_le_into_buffer(buffer, geometry->bytes_per_page);
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
	return ASSERT_LOG_ONLY((available_space(buffer) == 0),
			       "%zu bytes encoded, of %zu expected",
			       content_length(buffer),
			       buffer_length(buffer));
}

/**********************************************************************/
static int __must_check
encode_index_config_08_02(struct buffer *buffer, struct configuration *config)
{
	int result;
        struct geometry *geometry = config->geometry;

	result = put_uint32_le_into_buffer(buffer,
					   geometry->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   geometry->chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   geometry->sparse_chapters_per_volume);
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
	result = put_uint32_le_into_buffer(buffer, geometry->bytes_per_page);
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
	result = put_uint64_le_into_buffer(buffer, geometry->remapped_virtual);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer,
					   geometry->remapped_physical);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return ASSERT_LOG_ONLY((available_space(buffer) == 0),
			       "%zu bytes encoded, of %zu expected",
			       content_length(buffer),
			       buffer_length(buffer));
}

/**********************************************************************/
int write_config_contents(struct buffered_writer *writer,
			  struct configuration *config,
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
		result = make_buffer(sizeof(struct uds_configuration_8_02),
				     &buffer);
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
	config->nonce = conf->nonce;

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
void log_uds_configuration(struct configuration *conf)
{
	uds_log_debug("Configuration:");
	uds_log_debug("  Record pages per chapter:   %10u",
		      conf->geometry->record_pages_per_chapter);
	uds_log_debug("  Chapters per volume:        %10u",
		      conf->geometry->chapters_per_volume);
	uds_log_debug("  Sparse chapters per volume: %10u",
		      conf->geometry->sparse_chapters_per_volume);
	uds_log_debug("  Cache size (chapters):      %10u",
		      conf->cache_chapters);
	uds_log_debug("  Volume index mean delta:    %10u",
		      conf->volume_index_mean_delta);
	uds_log_debug("  Bytes per page:             %10zu",
		      conf->geometry->bytes_per_page);
	uds_log_debug("  Sparse sample rate:         %10u",
		      conf->sparse_sample_rate);
	uds_log_debug("  Nonce:                      %llu",
		      (unsigned long long) conf->nonce);
}
