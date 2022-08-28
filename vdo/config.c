// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "config.h"

#include "buffer.h"
#include "logger.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "uds-threads.h"

static const byte INDEX_CONFIG_MAGIC[] = "ALBIC";
static const byte INDEX_CONFIG_VERSION_6_02[] = "06.02";
static const byte INDEX_CONFIG_VERSION_8_02[] = "08.02";

enum {
	DEFAULT_VOLUME_READ_THREADS = 2,
	MAX_VOLUME_READ_THREADS = 16,
	INDEX_CONFIG_MAGIC_LENGTH = sizeof(INDEX_CONFIG_MAGIC) - 1,
	INDEX_CONFIG_VERSION_LENGTH = sizeof(INDEX_CONFIG_VERSION_6_02) - 1,
};

static int __must_check
decode_index_config_06_02(struct buffer *buffer,
			  struct uds_configuration_8_02 *config)
{
	int result;

	result = get_uint32_le_from_buffer(buffer,
					   &config->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer,
					   &config->chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer,
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

	result = get_uint32_le_from_buffer(buffer,
					   &config->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &config->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}

	config->remapped_virtual = 0;
	config->remapped_physical = 0;

	result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
				 "%zu bytes read but not decoded",
				 content_length(buffer));
	if (result != UDS_SUCCESS) {
		return UDS_CORRUPT_DATA;
	}

	return UDS_SUCCESS;
}

static int __must_check
decode_index_config_08_02(struct buffer *buffer,
			  struct uds_configuration_8_02 *config)
{
	int result;

	result = get_uint32_le_from_buffer(buffer,
					   &config->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer,
					   &config->chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer,
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

	result = get_uint32_le_from_buffer(buffer,
					   &config->sparse_sample_rate);
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

	result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
				 "%zu bytes read but not decoded",
				 content_length(buffer));
	if (result != UDS_SUCCESS) {
		return UDS_CORRUPT_DATA;
	}

	return result;
}

static bool is_version(const byte *version, byte *buffer)
{
	return (memcmp(version, buffer, INDEX_CONFIG_VERSION_LENGTH) == 0);
}

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

	if (is_version(INDEX_CONFIG_VERSION_6_02, version_buffer)) {
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
	} else if (is_version(INDEX_CONFIG_VERSION_8_02, version_buffer)) {
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
		result = UDS_CORRUPT_DATA;
	}

	return result;
}

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

/* Read the configuration and validate it against the provided one. */
int validate_config_contents(struct buffered_reader *reader,
			     struct configuration *config)
{
	int result;
	struct uds_configuration_8_02 saved;

	result = verify_buffered_data(reader,
				      INDEX_CONFIG_MAGIC,
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

/*
 * Write the configuration to stable storage. If the superblock
 * version is < 4, write the 6.02 version; otherwise write the 8.02
 * version, indicating the configuration is for an index that has been
 * reduced by one chapter.
 */
int write_config_contents(struct buffered_writer *writer,
			  struct configuration *config,
			  uint32_t version)
{
	int result;
	struct buffer *buffer;

	result = write_to_buffered_writer(writer,
					  INDEX_CONFIG_MAGIC,
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

	result = write_to_buffered_writer(writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	return result;
}

/* Compute configuration parameters that depend on memory size. */
static int compute_memory_sizes(uds_memory_config_size_t mem_gb,
				bool sparse,
				unsigned int *chapters_per_volume,
				unsigned int *record_pages_per_chapter,
				unsigned int *sparse_chapters_per_volume)
{
	unsigned int reduced_chapters = 0;
	unsigned int base_chapters;

	if (mem_gb == UDS_MEMORY_CONFIG_256MB) {
		base_chapters = DEFAULT_CHAPTERS_PER_VOLUME;
		*record_pages_per_chapter = SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_512MB) {
		base_chapters = DEFAULT_CHAPTERS_PER_VOLUME;
		*record_pages_per_chapter = 2 * SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_768MB) {
		base_chapters = DEFAULT_CHAPTERS_PER_VOLUME;
		*record_pages_per_chapter = 3 * SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if ((mem_gb >= 1) && (mem_gb <= UDS_MEMORY_CONFIG_MAX)) {
		base_chapters = mem_gb * DEFAULT_CHAPTERS_PER_VOLUME;
		*record_pages_per_chapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_REDUCED_256MB) {
		reduced_chapters = 1;
		base_chapters = DEFAULT_CHAPTERS_PER_VOLUME;
		*record_pages_per_chapter = SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_REDUCED_512MB) {
		reduced_chapters = 1;
		base_chapters = DEFAULT_CHAPTERS_PER_VOLUME;
		*record_pages_per_chapter = 2 * SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_REDUCED_768MB) {
		reduced_chapters = 1;
		base_chapters = DEFAULT_CHAPTERS_PER_VOLUME;
		*record_pages_per_chapter = 3 * SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if ((mem_gb >= 1 + UDS_MEMORY_CONFIG_REDUCED) &&
		   (mem_gb <= UDS_MEMORY_CONFIG_REDUCED_MAX)) {
		reduced_chapters = 1;
		base_chapters = ((mem_gb - UDS_MEMORY_CONFIG_REDUCED) *
				 DEFAULT_CHAPTERS_PER_VOLUME);
		*record_pages_per_chapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
	} else {
		uds_log_error("received invalid memory size");
		return -EINVAL;
	}

	if (sparse) {
		/* Make 95% of chapters sparse, allowing 10x more records. */
		*sparse_chapters_per_volume = (19 * base_chapters) / 2;
		base_chapters *= 10;
	} else {
		*sparse_chapters_per_volume = 0;
	}

	*chapters_per_volume = base_chapters - reduced_chapters;
	return UDS_SUCCESS;
}

static unsigned int __must_check normalize_zone_count(unsigned int requested)
{
	unsigned int zone_count = requested;

	if (zone_count == 0) {
		zone_count = uds_get_num_cores() / 2;
	}

	if (zone_count < 1) {
		zone_count = 1;
	}

	if (zone_count > MAX_ZONES) {
		zone_count = MAX_ZONES;
	}

	uds_log_info("Using %u indexing zone%s for concurrency.",
		     zone_count,
		     zone_count == 1 ? "" : "s");
	return zone_count;
}

static unsigned int __must_check normalize_read_threads(unsigned int requested)
{
	unsigned int read_threads = requested;

	if (read_threads < 1) {
		read_threads = DEFAULT_VOLUME_READ_THREADS;
	}

	if (read_threads > MAX_VOLUME_READ_THREADS) {
		read_threads = MAX_VOLUME_READ_THREADS;
	}

	return read_threads;
}

int make_configuration(const struct uds_parameters *params,
		       struct configuration **config_ptr)
{
	struct configuration *config;
	unsigned int chapters_per_volume = 0;
	unsigned int record_pages_per_chapter = 0;
	unsigned int sparse_chapters_per_volume = 0;
	int result;

	result = compute_memory_sizes(params->memory_size,
				      params->sparse,
				      &chapters_per_volume,
				      &record_pages_per_chapter,
				      &sparse_chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(1, struct configuration, __func__, &config);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = make_geometry(DEFAULT_BYTES_PER_PAGE,
			       record_pages_per_chapter,
			       chapters_per_volume,
			       sparse_chapters_per_volume,
			       0,
			       0,
			       &config->geometry);
	if (result != UDS_SUCCESS) {
		free_configuration(config);
		return result;
	}

	config->zone_count = normalize_zone_count(params->zone_count);
	config->read_threads = normalize_read_threads(params->read_threads);

	config->cache_chapters = DEFAULT_CACHE_CHAPTERS;
	config->volume_index_mean_delta =
		DEFAULT_VOLUME_INDEX_MEAN_DELTA;
	config->sparse_sample_rate =
		(params->sparse ? DEFAULT_SPARSE_SAMPLE_RATE : 0);
	config->nonce = params->nonce;
	config->name = params->name;
	config->offset = params->offset;
	config->size = params->size;

	*config_ptr = config;
	return UDS_SUCCESS;
}

void free_configuration(struct configuration *config)
{
	if (config != NULL) {
		free_geometry(config->geometry);
		UDS_FREE(config);
	}
}

void log_uds_configuration(struct configuration *config)
{
	uds_log_debug("Configuration:");
	uds_log_debug("  Record pages per chapter:   %10u",
		      config->geometry->record_pages_per_chapter);
	uds_log_debug("  Chapters per volume:        %10u",
		      config->geometry->chapters_per_volume);
	uds_log_debug("  Sparse chapters per volume: %10u",
		      config->geometry->sparse_chapters_per_volume);
	uds_log_debug("  Cache size (chapters):      %10u",
		      config->cache_chapters);
	uds_log_debug("  Volume index mean delta:    %10u",
		      config->volume_index_mean_delta);
	uds_log_debug("  Bytes per page:             %10zu",
		      config->geometry->bytes_per_page);
	uds_log_debug("  Sparse sample rate:         %10u",
		      config->sparse_sample_rate);
	uds_log_debug("  Nonce:                      %llu",
		      (unsigned long long) config->nonce);
}
