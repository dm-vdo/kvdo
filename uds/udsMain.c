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
 * $Id: //eng/uds-releases/krusty/src/uds/udsMain.c#3 $
 */

#include "uds.h"

#include "config.h"
#include "geometry.h"
#include "indexLayout.h"
#include "indexRouter.h"
#include "indexSession.h"
#include "loadType.h"
#include "logger.h"
#include "memoryAlloc.h"

const UdsMemoryConfigSize UDS_MEMORY_CONFIG_MAX   = 1024;
const UdsMemoryConfigSize UDS_MEMORY_CONFIG_256MB = (UdsMemoryConfigSize) -256;
const UdsMemoryConfigSize UDS_MEMORY_CONFIG_512MB = (UdsMemoryConfigSize) -512;
const UdsMemoryConfigSize UDS_MEMORY_CONFIG_768MB = (UdsMemoryConfigSize) -768;

/*
 * ===========================================================================
 * UDS system management
 * ===========================================================================
 */

/**********************************************************************/
int udsInitializeConfiguration(UdsConfiguration    *userConfig,
                               UdsMemoryConfigSize  memGB)
{
  if (userConfig == NULL) {
    return logErrorWithStringError(UDS_CONF_PTR_REQUIRED,
                                   "received a NULL config pointer");
  }

  /* Set the configuration parameters that change with memory size.  If you
   * change these values, you should also:
   *
   * Change Configuration_x1, which tests these values and expects to see them
   *
   * Bump the index configuration version number.  This bump ensures that
   * the test infrastructure will be forced to test the new configuration.
   */

  unsigned int chaptersPerVolume, recordPagesPerChapter;
  if (memGB == UDS_MEMORY_CONFIG_256MB) {
    chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = SMALL_RECORD_PAGES_PER_CHAPTER;
  } else if (memGB == UDS_MEMORY_CONFIG_512MB) {
    chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = 2 * SMALL_RECORD_PAGES_PER_CHAPTER;
  } else if (memGB == UDS_MEMORY_CONFIG_768MB) {
    chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = 3 * SMALL_RECORD_PAGES_PER_CHAPTER;
  } else if (memGB == 1) {
    chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
  } else if ((memGB > 1) && (memGB <= UDS_MEMORY_CONFIG_MAX)) {
    chaptersPerVolume     = memGB * DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
  } else {
    return UDS_INVALID_MEMORY_SIZE;
  }

  int result = ALLOCATE(1, struct udsConfiguration, "udsConfiguration",
                        userConfig);
  if (result != UDS_SUCCESS) {
    return result;
  }

  (*userConfig)->recordPagesPerChapter   = recordPagesPerChapter;
  (*userConfig)->chaptersPerVolume       = chaptersPerVolume;
  (*userConfig)->sparseChaptersPerVolume = DEFAULT_SPARSE_CHAPTERS_PER_VOLUME;
  (*userConfig)->cacheChapters           = DEFAULT_CACHE_CHAPTERS;
  (*userConfig)->checkpointFrequency     = DEFAULT_CHECKPOINT_FREQUENCY;
  (*userConfig)->masterIndexMeanDelta    = DEFAULT_MASTER_INDEX_MEAN_DELTA;
  (*userConfig)->bytesPerPage            = DEFAULT_BYTES_PER_PAGE;
  (*userConfig)->sparseSampleRate        = DEFAULT_SPARSE_SAMPLE_RATE;
  (*userConfig)->nonce                   = 0;
  return UDS_SUCCESS;
}

/**********************************************************************/
void udsConfigurationSetSparse(UdsConfiguration userConfig, bool sparse)
{
  bool prevSparse = (userConfig->sparseChaptersPerVolume != 0);
  if (sparse == prevSparse) {
    // nothing to do
    return;
  }

  unsigned int prevChaptersPerVolume = userConfig->chaptersPerVolume;
  if (sparse) {
    // Index 10TB with 4K blocks, 95% sparse, fit in dense (1TB) footprint
    userConfig->chaptersPerVolume = 10 * prevChaptersPerVolume;
    userConfig->sparseChaptersPerVolume = 9 * prevChaptersPerVolume
      + prevChaptersPerVolume / 2;
    userConfig->sparseSampleRate = 32;
  } else {
    userConfig->chaptersPerVolume = prevChaptersPerVolume / 10;
    userConfig->sparseChaptersPerVolume = 0;
    userConfig->sparseSampleRate = 0;
  }
}

/**********************************************************************/
bool udsConfigurationGetSparse(UdsConfiguration userConfig)
{
  return userConfig->sparseChaptersPerVolume > 0;
}

/**********************************************************************/
void udsConfigurationSetNonce(UdsConfiguration userConfig, UdsNonce nonce)
{
  userConfig->nonce = nonce;
}

/**********************************************************************/
UdsNonce udsConfigurationGetNonce(UdsConfiguration userConfig)
{
  return userConfig->nonce;
}

/**********************************************************************/
unsigned int udsConfigurationGetMemory(UdsConfiguration userConfig)
{
  enum {
    CHAPTERS = DEFAULT_CHAPTERS_PER_VOLUME,
    SMALL_PAGES = CHAPTERS * SMALL_RECORD_PAGES_PER_CHAPTER,
    LARGE_PAGES = CHAPTERS * DEFAULT_RECORD_PAGES_PER_CHAPTER
  };
  unsigned int pages = (userConfig->chaptersPerVolume
                        * userConfig->recordPagesPerChapter);
  if (userConfig->sparseChaptersPerVolume != 0) {
    pages /= 10;
  }
  switch (pages) {
  case SMALL_PAGES:     return UDS_MEMORY_CONFIG_256MB;
  case 2 * SMALL_PAGES: return UDS_MEMORY_CONFIG_512MB;
  case 3 * SMALL_PAGES: return UDS_MEMORY_CONFIG_768MB;
  default:              return pages / LARGE_PAGES;
  }
}

/**********************************************************************/
unsigned int
udsConfigurationGetChaptersPerVolume(UdsConfiguration userConfig)
{
  return userConfig->chaptersPerVolume;
}

/**********************************************************************/
void udsFreeConfiguration(UdsConfiguration userConfig)
{
  FREE(userConfig);
}

/**********************************************************************/
int udsCreateIndexSession(struct uds_index_session **session)
{
  if (session == NULL) {
    return UDS_NO_INDEXSESSION;
  }

  struct uds_index_session *indexSession = NULL;
  int result = makeEmptyIndexSession(&indexSession);
  if (result != UDS_SUCCESS) {
    return result;
  }

  *session = indexSession;
  return UDS_SUCCESS;
}

/**********************************************************************/
static
int initializeIndexSessionWithLayout(struct uds_index_session    *indexSession,
                                     struct index_layout         *layout,
                                     const struct uds_parameters *userParams,
                                     LoadType                     loadType)
{
  int result = ((loadType == LOAD_CREATE)
                ? write_index_config(layout, &indexSession->userConfig)
                : verify_index_config(layout, &indexSession->userConfig));
  if (result != UDS_SUCCESS) {
    return result;
  }

  Configuration *indexConfig;
  result = makeConfiguration(&indexSession->userConfig, &indexConfig);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Failed to allocate config");
    return result;
  }

  // Zero the stats for the new index.
  memset(&indexSession->stats, 0, sizeof(indexSession->stats));

  result = makeIndexRouter(layout, indexConfig, userParams, loadType,
                           &indexSession->loadContext, enterCallbackStage,
                           &indexSession->router);
  freeConfiguration(indexConfig);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Failed to make router");
    return result;
  }

  logUdsConfiguration(&indexSession->userConfig);
  return UDS_SUCCESS;
}

/**********************************************************************/
static int initializeIndexSession(struct uds_index_session    *indexSession,
                                  const char                  *name,
                                  const struct uds_parameters *userParams,
                                  LoadType                     loadType)
{
  struct index_layout *layout;
  int result = make_index_layout(name, loadType == LOAD_CREATE,
                                 &indexSession->userConfig, &layout);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initializeIndexSessionWithLayout(indexSession, layout, userParams,
                                            loadType);
  put_index_layout(&layout);
  return result;
}

/**********************************************************************/
int udsOpenIndex(UdsOpenIndexType             openType,
                 const char                  *name,
                 const struct uds_parameters *userParams,
                 UdsConfiguration             userConfig,
                 struct uds_index_session    *session)
{
  if (name == NULL) {
    return UDS_INDEX_NAME_REQUIRED;
  }
  if (userConfig == NULL) {
    return UDS_CONF_REQUIRED;
  }
  if (session == NULL) {
    return UDS_NO_INDEXSESSION;
  }

  int result = startLoadingIndexSession(session);
  if (result != UDS_SUCCESS) {
    return result;
  }

  session->userConfig = *userConfig;

  // Map the external openType to the internal loadType
  LoadType loadType =   openType == UDS_CREATE     ? LOAD_CREATE
                      : openType == UDS_NO_REBUILD ? LOAD_LOAD
                      :                              LOAD_REBUILD;
  logNotice("%s: %s", getLoadType(loadType), name);

  result = initializeIndexSession(session, name, userParams, loadType);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Failed %s", getLoadType(loadType));
    saveAndFreeIndex(session);
  }

  finishLoadingIndexSession(session, result);
  return sansUnrecoverable(result);
}

/**********************************************************************/
const char *udsGetVersion(void)
{
#ifdef UDS_VERSION
  return UDS_VERSION;
#else
  return "internal version";
#endif
}

/**********************************************************************/
const char *udsStringError(int errnum, char *buf, size_t buflen)
{
  if (buf == NULL) {
    return NULL;
  }

  return stringError(errnum, buf, buflen);
}
