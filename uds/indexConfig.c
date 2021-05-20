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
 * $Id: //eng/uds-releases/jasper/src/uds/indexConfig.c#4 $
 */

#include "indexConfig.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"

static const byte INDEX_CONFIG_MAGIC[] = "ALBIC";
static const byte INDEX_CONFIG_VERSION_6_02[] = "06.02";
static const byte INDEX_CONFIG_VERSION_8_02[] = "08.02";

enum {
  INDEX_CONFIG_MAGIC_LENGTH   = sizeof(INDEX_CONFIG_MAGIC) - 1,
  INDEX_CONFIG_VERSION_LENGTH = sizeof(INDEX_CONFIG_VERSION_6_02) - 1
};

/**********************************************************************/
__attribute__((warn_unused_result))
static int decodeIndexConfig_06_02(Buffer *buffer, UdsConfiguration config)
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
  config->remappedChapter = 0;
  config->chapterOffset = 0;
  if (ASSERT_LOG_ONLY(contentLength(buffer) == 0,
                      "%zu bytes decoded of %zu expected",
                      bufferLength(buffer) - contentLength(buffer),
                      bufferLength(buffer)) != UDS_SUCCESS) {
    return UDS_CORRUPT_COMPONENT;
  }
  return result;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int decodeIndexConfig_08_02(Buffer *buffer, UdsConfiguration config)
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
  result = getUInt64LEFromBuffer(buffer, &config->remappedChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &config->chapterOffset);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (ASSERT_LOG_ONLY(contentLength(buffer) == 0,
                      "%zu bytes decoded of %zu expected",
                      bufferLength(buffer) - contentLength(buffer),
                      bufferLength(buffer)) != UDS_SUCCESS) {
    return UDS_CORRUPT_COMPONENT;
  }
  return result;
}

/**********************************************************************/
static int readVersion(BufferedReader   *reader,
                       UdsConfiguration  conf)
{
  byte versionBuffer[INDEX_CONFIG_VERSION_LENGTH];
  int result = readFromBufferedReader(reader, versionBuffer,
                                      INDEX_CONFIG_VERSION_LENGTH);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "cannot read index config version");
  }
  if (memcmp(INDEX_CONFIG_VERSION_6_02, versionBuffer,
             INDEX_CONFIG_VERSION_LENGTH) == 0) {
    Buffer *buffer;
    result = makeBuffer(sizeof(struct udsConfiguration_6_02), &buffer);
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
    result = decodeIndexConfig_06_02(buffer, conf);
    freeBuffer(&buffer);
  } else if (memcmp(INDEX_CONFIG_VERSION_8_02, versionBuffer,
                    INDEX_CONFIG_VERSION_LENGTH) == 0) {
    Buffer *buffer;
    result = makeBuffer(sizeof(struct udsConfiguration), &buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = readFromBufferedReader(reader,
                                    getBufferContents(buffer),
                                    bufferLength(buffer));
    if (result != UDS_SUCCESS) {
      freeBuffer(&buffer);
      return logErrorWithStringError(result,
                                     "cannot read config data");
    }
    clearBuffer(buffer);
    result = decodeIndexConfig_08_02(buffer, conf);
    freeBuffer(&buffer);
  } else {
    logErrorWithStringError(result,
                            "unsupported configuration version: '%.*s'",
                            INDEX_CONFIG_VERSION_LENGTH,
                            versionBuffer);
    result = UDS_CORRUPT_COMPONENT;
  }
  return result;
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

  result = readVersion(reader, config);
  if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "Failed to read index config");
  }
  return result;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int encodeIndexConfig_06_02(Buffer *buffer, UdsConfiguration config)
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
  return ASSERT_LOG_ONLY(contentLength(buffer) ==
                         sizeof(struct udsConfiguration_6_02),
                         "%zu bytes encoded, of %zu expected",
                         contentLength(buffer),
                         sizeof(*config));
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int encodeIndexConfig_08_02(Buffer *buffer, UdsConfiguration config)
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
  result = putUInt64LEIntoBuffer(buffer, config->remappedChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }  
  result = putUInt64LEIntoBuffer(buffer, config->chapterOffset);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return ASSERT_LOG_ONLY(contentLength(buffer) ==
                         sizeof(struct udsConfiguration),
                         "%zu bytes encoded, of %zu expected",
                         contentLength(buffer),
                         sizeof(*config));
}

/**********************************************************************/
int writeConfigContents(BufferedWriter *writer,
                        UdsConfiguration config,
                        uint32_t version)
{
  Buffer *buffer;
  int result = writeToBufferedWriter(writer, INDEX_CONFIG_MAGIC,
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
    result = writeToBufferedWriter(writer,
                                   INDEX_CONFIG_VERSION_6_02,
                                   INDEX_CONFIG_VERSION_LENGTH);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = makeBuffer(sizeof(struct udsConfiguration_6_02),
                        &buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = encodeIndexConfig_06_02(buffer, config);
    if (result != UDS_SUCCESS) {
      freeBuffer(&buffer);
      return result;
    }
  } else {
    result = writeToBufferedWriter(writer,
                                   INDEX_CONFIG_VERSION_8_02,
                                   INDEX_CONFIG_VERSION_LENGTH);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = makeBuffer(sizeof(struct udsConfiguration), &buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = encodeIndexConfig_08_02(buffer, config);
    if (result != UDS_SUCCESS) {
      freeBuffer(&buffer);
      return result;
    }
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
                        conf->remappedChapter,
                        conf->chapterOffset,
                        &config->geometry);
  if (result != UDS_SUCCESS) {
    freeConfiguration(config);
    return result;
  }

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
