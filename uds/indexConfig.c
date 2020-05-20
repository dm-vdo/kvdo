/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/krusty/src/uds/indexConfig.c#10 $
 */

#include "indexConfig.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"

static const byte INDEX_CONFIG_MAGIC[]        = "ALBIC";
static const byte INDEX_CONFIG_VERSION[]      = "06.02";
static const byte INDEX_CONFIG_VERSION_6_01[] = "06.01";

enum {
  INDEX_CONFIG_MAGIC_LENGTH   = sizeof(INDEX_CONFIG_MAGIC) - 1,
  INDEX_CONFIG_VERSION_LENGTH = sizeof(INDEX_CONFIG_VERSION) - 1
};

/**********************************************************************/
static int __must_check
decodeIndexConfig(struct buffer *buffer, struct uds_configuration *config)
{
  int result = get_uint32_le_from_buffer(buffer,
                                         &config->record_pages_per_chapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint32_le_from_buffer(buffer, &config->chapters_per_volume);
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
  result = get_uint32_le_from_buffer(buffer, &config->checkpoint_frequency);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint32_le_from_buffer(buffer, &config->master_index_mean_delta);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint32_le_from_buffer(buffer, &config->bytes_per_page);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint32_le_from_buffer(buffer, &config->sparse_sample_rate);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint64_le_from_buffer(buffer, &config->nonce);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
                           "%zu bytes decoded of %zu expected",
                           buffer_length(buffer) - content_length(buffer),
                           buffer_length(buffer));
  if (result != UDS_SUCCESS) {
    result = UDS_CORRUPT_COMPONENT;
  }
  return result;
}

/**********************************************************************/
static int readVersion(struct buffered_reader    *reader,
                       struct uds_configuration  *conf,
                       const char               **versionPtr)
{
  byte buffer[INDEX_CONFIG_VERSION_LENGTH];
  int result = read_from_buffered_reader(reader, buffer,
                                         INDEX_CONFIG_VERSION_LENGTH);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "cannot read index config version");
  }
  if (memcmp(INDEX_CONFIG_VERSION, buffer, INDEX_CONFIG_VERSION_LENGTH) == 0) {
    struct buffer *buffer;
    result = make_buffer(sizeof(*conf), &buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = read_from_buffered_reader(reader, get_buffer_contents(buffer),
                                       buffer_length(buffer));
    if (result != UDS_SUCCESS) {
      free_buffer(&buffer);
      return logErrorWithStringError(result, "cannot read config data");
    }
    clear_buffer(buffer);
    result = decodeIndexConfig(buffer, conf);
    free_buffer(&buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (versionPtr != NULL) {
      *versionPtr = "current";
    }
    return result;
  } else if (memcmp(INDEX_CONFIG_VERSION_6_01, buffer,
                    INDEX_CONFIG_VERSION_LENGTH) == 0) {
    struct uds_configuration_6_01 oldConf;
    result = read_from_buffered_reader(reader, &oldConf, sizeof(oldConf));
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result,
                              "failed to read version 6.01 config file");
      return result;
    }
    conf->record_pages_per_chapter   = oldConf.record_pages_per_chapter;
    conf->chapters_per_volume        = oldConf.chapters_per_volume;
    conf->sparse_chapters_per_volume = oldConf.sparse_chapters_per_volume;
    conf->cache_chapters             = oldConf.cache_chapters;
    conf->checkpoint_frequency       = oldConf.checkpoint_frequency;
    conf->master_index_mean_delta    = oldConf.master_index_mean_delta;
    conf->bytes_per_page             = oldConf.bytes_per_page;
    conf->sparse_sample_rate         = oldConf.sparse_sample_rate;
    conf->nonce                   = 0;
    if (versionPtr != NULL) {
      *versionPtr = "6.01";
    }
    return UDS_UNSUPPORTED_VERSION;
  }

  return logErrorWithStringError(UDS_CORRUPT_COMPONENT,
                                 "unsupported configuration version: '%.*s'",
                                 INDEX_CONFIG_VERSION_LENGTH, buffer);
}

/**********************************************************************/
int read_config_contents(struct buffered_reader   *reader,
                         struct uds_configuration *config)
{
  int result = verify_buffered_data(reader, INDEX_CONFIG_MAGIC,
                                    INDEX_CONFIG_MAGIC_LENGTH);
  if (result != UDS_SUCCESS) {
    return result;
  }

  const char *version = NULL;
  result = readVersion(reader, config, &version);
  if (result != UDS_SUCCESS) {
    if (result == UDS_UNSUPPORTED_VERSION) {
      logNoticeWithStringError(result, "Found index config version %s",
                               version);
    } else {
      logErrorWithStringError(result, "Failed to read index config");
    }
  }
  return result;
}

/**********************************************************************/
static int __must_check
encodeIndexConfig(struct buffer *buffer, struct uds_configuration *config)
{
  int result = put_uint32_le_into_buffer(buffer,
                                         config->record_pages_per_chapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_uint32_le_into_buffer(buffer, config->chapters_per_volume);
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
  result = put_uint32_le_into_buffer(buffer, config-> checkpoint_frequency);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_uint32_le_into_buffer(buffer, config->master_index_mean_delta);
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
  result = ASSERT_LOG_ONLY(content_length(buffer) == sizeof(*config),
                           "%zu bytes encoded, of %zu expected",
                           content_length(buffer), sizeof(*config));
  return result;
}

/**********************************************************************/
int write_config_contents(struct buffered_writer   *writer,
                          struct uds_configuration *config)
{
  int result = write_to_buffered_writer(writer, INDEX_CONFIG_MAGIC,
                                        INDEX_CONFIG_MAGIC_LENGTH);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = write_to_buffered_writer(writer, INDEX_CONFIG_VERSION,
                                    INDEX_CONFIG_VERSION_LENGTH);
  if (result != UDS_SUCCESS) {
    return result;
  }
  struct buffer *buffer;
  result = make_buffer(sizeof(*config), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = encodeIndexConfig(buffer, config);
  if (result != UDS_SUCCESS) {
    free_buffer(&buffer);
    return result;
  }
  result = write_to_buffered_writer(writer, get_buffer_contents(buffer),
                                    content_length(buffer));
  free_buffer(&buffer);
  return result;
}

/**********************************************************************/
int make_configuration(const struct uds_configuration *conf,
                       struct configuration **configPtr)
{
  *configPtr = NULL;
  if (conf == NULL) {
    return logErrorWithStringError(UDS_CONF_REQUIRED,
                                   "received an invalid config");
  }

  struct configuration *config;
  int result = ALLOCATE(1, struct configuration, "configuration", &config);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = makeGeometry(conf->bytes_per_page,
                        conf->record_pages_per_chapter,
                        conf->chapters_per_volume,
                        conf->sparse_chapters_per_volume,
                        &config->geometry);
  if (result != UDS_SUCCESS) {
    free_configuration(config);
    return result;
  }

  config->sparseSampleRate     = conf->sparse_sample_rate;
  config->cacheChapters        = conf->cache_chapters;
  config->masterIndexMeanDelta = conf->master_index_mean_delta;

  *configPtr = config;
  return UDS_SUCCESS;
}

/**********************************************************************/
void free_configuration(struct configuration *config)
{
  if (config != NULL) {
    freeGeometry(config->geometry);
    FREE(config);
  }
}
