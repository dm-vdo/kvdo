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
 * $Id: //eng/uds-releases/gloria/src/uds/indexConfig.c#4 $
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
__attribute__((warn_unused_result))
static int decodeIndexConfig(Buffer *buffer, UdsConfiguration config)
{
  int result = getUInt32LEFromBuffer(buffer, &config->recordPagesPerChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &config->chaptersPerVolume);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &config->sparseChaptersPerVolume);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &config->cacheChapters);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &config->checkpointFrequency);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &config->masterIndexMeanDelta);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &config->bytesPerPage);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &config->sparseSampleRate);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &config->nonce);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == 0,
                           "%zu bytes decoded of %zu expected",
                           bufferLength(buffer) - contentLength(buffer),
                           bufferLength(buffer));
  if (result != UDS_SUCCESS) {
    result = UDS_CORRUPT_COMPONENT;
  }
  return result;
}

/**********************************************************************/
static int readVersion(BufferedReader    *reader,
                       UdsConfiguration   conf,
                       const char       **versionPtr)
{
  byte buffer[INDEX_CONFIG_VERSION_LENGTH];
  int result = readFromBufferedReader(reader, buffer,
                                      INDEX_CONFIG_VERSION_LENGTH);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "cannot read index config version");
  }
  if (memcmp(INDEX_CONFIG_VERSION, buffer, INDEX_CONFIG_VERSION_LENGTH) == 0) {
    Buffer *buffer;
    result = makeBuffer(sizeof(*conf), &buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = readFromBufferedReader(reader, getBufferContents(buffer),
                                    bufferLength(buffer));
    if (result != UDS_SUCCESS) {
      freeBuffer(&buffer);
      return logErrorWithStringError(result, "cannot read config data");
    }
    clearBuffer(buffer);
    result = decodeIndexConfig(buffer, conf);
    freeBuffer(&buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (versionPtr != NULL) {
      *versionPtr = "current";
    }
    return result;
  } else if (memcmp(INDEX_CONFIG_VERSION_6_01, buffer,
                    INDEX_CONFIG_VERSION_LENGTH) == 0) {
    struct udsConfiguration6_01 oldConf;
    result = readFromBufferedReader(reader, &oldConf, sizeof(oldConf));
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result,
                              "failed to read version 6.01 config file");
      return result;
    }
    conf->recordPagesPerChapter   = oldConf.recordPagesPerChapter;
    conf->chaptersPerVolume       = oldConf.chaptersPerVolume;
    conf->sparseChaptersPerVolume = oldConf.sparseChaptersPerVolume;
    conf->cacheChapters           = oldConf.cacheChapters;
    conf->checkpointFrequency     = oldConf.checkpointFrequency;
    conf->masterIndexMeanDelta    = oldConf.masterIndexMeanDelta;
    conf->bytesPerPage            = oldConf.bytesPerPage;
    conf->sparseSampleRate        = oldConf.sparseSampleRate;
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
int readConfigContents(BufferedReader   *reader,
                       UdsConfiguration  config)
{
  int result = verifyBufferedData(reader, INDEX_CONFIG_MAGIC,
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
__attribute__((warn_unused_result))
static int encodeIndexConfig(Buffer *buffer, UdsConfiguration config)
{
  int result = putUInt32LEIntoBuffer(buffer, config->recordPagesPerChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, config->chaptersPerVolume);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, config->sparseChaptersPerVolume);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, config->cacheChapters);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, config-> checkpointFrequency);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, config->masterIndexMeanDelta);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, config->bytesPerPage);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, config->sparseSampleRate);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, config->nonce);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == sizeof(*config),
                           "%zu bytes encoded, of %zu expected",
                           contentLength(buffer), sizeof(*config));
  return result;
}

/**********************************************************************/
int writeConfigContents(BufferedWriter   *writer,
                        UdsConfiguration  config)
{
  int result = writeToBufferedWriter(writer, INDEX_CONFIG_MAGIC,
                                     INDEX_CONFIG_MAGIC_LENGTH);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = writeToBufferedWriter(writer, INDEX_CONFIG_VERSION,
                                 INDEX_CONFIG_VERSION_LENGTH);
  if (result != UDS_SUCCESS) {
    return result;
  }
  Buffer *buffer;
  result = makeBuffer(sizeof(*config), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = encodeIndexConfig(buffer, config);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = writeToBufferedWriter(writer, getBufferContents(buffer),
                                 contentLength(buffer));
  freeBuffer(&buffer);
  return result;
}

/**********************************************************************/
int makeConfiguration(UdsConfiguration conf, Configuration **configPtr)
{
  *configPtr = NULL;
  if (conf == NULL) {
    return logErrorWithStringError(UDS_CONF_REQUIRED,
                                   "received an invalid config");
  }

  Configuration *config;
  int result = ALLOCATE(1, Configuration, "configuration", &config);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = makeGeometry(conf->bytesPerPage,
                        conf->recordPagesPerChapter,
                        conf->chaptersPerVolume,
                        conf->sparseChaptersPerVolume,
                        &config->geometry);
  if (result != UDS_SUCCESS) {
    freeConfiguration(config);
    return result;
  }

  config->checkpointFrequency  = conf->checkpointFrequency;
  config->sparseSampleRate     = conf->sparseSampleRate;
  config->cacheChapters        = conf->cacheChapters;
  config->masterIndexMeanDelta = conf->masterIndexMeanDelta;

  *configPtr = config;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeConfiguration(Configuration *config)
{
  if (config != NULL) {
    freeGeometry(config->geometry);
    FREE(config);
  }
}
