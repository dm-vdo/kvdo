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
 * $Id: //eng/uds-releases/gloria/src/uds/udsMain.c#3 $
 */

#include "uds.h"

#include "config.h"
#include "featureDefs.h"
#include "geometry.h"
#include "grid.h"
#include "indexLayout.h"
#include "indexSession.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "udsState.h"

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
   * the test infrastructure will be forced to test the new configuration,
   * and also prevents a grid trying to run with both old and new
   * configurations on different servers.
   */

  unsigned int chaptersPerVolume, recordPagesPerChapter, checkpointFrequency;
  if (memGB == UDS_MEMORY_CONFIG_256MB) {
    chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = SMALL_RECORD_PAGES_PER_CHAPTER;
    checkpointFrequency   = DEFAULT_CHECKPOINT_FREQUENCY;
  } else if (memGB == UDS_MEMORY_CONFIG_512MB) {
    chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = 2 * SMALL_RECORD_PAGES_PER_CHAPTER;
    checkpointFrequency   = DEFAULT_CHECKPOINT_FREQUENCY;
  } else if (memGB == UDS_MEMORY_CONFIG_768MB) {
    chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = 3 * SMALL_RECORD_PAGES_PER_CHAPTER;
    checkpointFrequency   = DEFAULT_CHECKPOINT_FREQUENCY;
  } else if (memGB == 1) {
    chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
    checkpointFrequency   = DEFAULT_CHECKPOINT_FREQUENCY;
  } else if ((memGB > 1) && (memGB <= UDS_MEMORY_CONFIG_MAX)) {
    chaptersPerVolume     = memGB * DEFAULT_CHAPTERS_PER_VOLUME;
    recordPagesPerChapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
    checkpointFrequency   = memGB * DEFAULT_CHECKPOINT_FREQUENCY;
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
  (*userConfig)->checkpointFrequency     = checkpointFrequency;
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
int udsConfigurationSetCheckpointFrequency(
  UdsConfiguration userConfig,
  unsigned int checkpointFrequency)
{
  if (checkpointFrequency > userConfig->chaptersPerVolume) {
    return UDS_BAD_CHECKPOINT_FREQUENCY;
  }
  userConfig->checkpointFrequency = checkpointFrequency;
  return UDS_SUCCESS;
}

/**********************************************************************/
unsigned int udsConfigurationGetCheckpointFrequency(
  UdsConfiguration userConfig)
{
  return userConfig->checkpointFrequency;
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
static int initializeIndexSession(IndexSession     *indexSession,
                                  UdsGridConfig     gridConfig,
                                  LoadType          loadType,
                                  UdsConfiguration  userConfig)
{
  int result;
  if (gridConfig->numLocations == 0) {
    return logErrorWithStringError(UDS_GRID_NO_SERVERS,
                                   "No locations specified");
  }

  logGridConfig(gridConfig, getLoadType(loadType));

  if (loadType == LOAD_ATTACH) {
#if GRID
    result = makeRemoteGrid(gridConfig, enterCallbackStage,
                            &indexSession->grid);
#else /* !GRID */
    result = UDS_UNSUPPORTED;
#endif /* GRID */
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "Failed to initialize grid");
      return result;
    }
  } else {
    IndexLayout *layout;
    result = makeIndexLayout(gridConfig->locations[0].directory,
                             loadType == LOAD_CREATE, userConfig, &layout);
    if (result == UDS_SUCCESS) {
      result = makeLocalGrid(layout, loadType, userConfig, enterCallbackStage,
                             &indexSession->grid);
      if (result == UDS_SUCCESS) {
        // on success, the layout becomes owned by the grid...
        result = removeSafetySeal(layout);
      } else {
        // on failure, we must free the layout
        freeIndexLayout(&layout);
      }
    }
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "Failed %s", getLoadType(loadType));
      return result;
    }
  }

  setIndexSessionState(indexSession, IS_READY);
  return UDS_SUCCESS;
}

/**********************************************************************/
static int makeIndexSession(UdsGridConfig     gridConfig,
                            LoadType          loadType,
                            UdsConfiguration  userConfig,
                            unsigned int     *indexSessionID)
{
  if (indexSessionID == NULL) {
    return UDS_NO_INDEXSESSION;
  }
  udsInitialize();

  lockGlobalStateMutex();
  int result = checkLibraryRunning();
  if (result != UDS_SUCCESS) {
    unlockGlobalStateMutex();
    return result;
  }

  // Hold a session group reference until the index seession is
  // completely initialized or cleaned up.
  SessionGroup *indexSessionGroup = getIndexSessionGroup();
  result = acquireSessionGroup(indexSessionGroup);
  if (result != UDS_SUCCESS) {
    unlockGlobalStateMutex();
    return result;
  }

  IndexSession *indexSession = NULL;
  result = makeEmptyIndexSession(&indexSession);
  if (result != UDS_SUCCESS) {
    releaseSessionGroup(indexSessionGroup);
    unlockGlobalStateMutex();
    return result;
  }

  SessionID id;
  result = initializeSession(indexSessionGroup, &indexSession->session,
                             (SessionContents) indexSession, &id);
  if (result != UDS_SUCCESS) {
    saveAndFreeIndexSession(indexSession);
    releaseSessionGroup(indexSessionGroup);
    unlockGlobalStateMutex();
    return result;
  }
  // Release state mutex because loading index/network connection can
  // take a long time. Shutdown still cannot proceed until reference
  // to base context is released below
  unlockGlobalStateMutex();

  result = initializeIndexSession(indexSession, gridConfig, loadType,
                                  userConfig);

  lockGlobalStateMutex();
  if (result != UDS_SUCCESS) {
    finishSession(indexSessionGroup, &indexSession->session);
    saveAndFreeIndexSession(indexSession);
    releaseSessionGroup(indexSessionGroup);
    unlockGlobalStateMutex();
    return handleError(NULL, result);
  }
  *indexSessionID = id;
  logDebug("Created index session (%u)", id);
  releaseIndexSession(indexSession);
  releaseSessionGroup(indexSessionGroup);
  unlockGlobalStateMutex();
  return UDS_SUCCESS;
}

// ============================================================================
// INDEX SESSION API
// ============================================================================

/**
 * Make a local index.
 *
 * @param name        The name of the index directory
 * @param loadType    How to create the index. It can be create only,
 *                    allow loading from files, and allow rebuilding
 *                    from the volume
 * @param userConfig  The configuration of the index
 * @param session     An index session connected to connect to the index
 *
 * @return UDS_SUCCESS or an error
 **/
static int makeLocalIndex(const char       *name,
                          LoadType          loadType,
                          UdsConfiguration  userConfig,
                          UdsIndexSession  *session)
{
  if (name == NULL) {
    return UDS_INDEX_NAME_REQUIRED;
  }
  if (session == NULL) {
    return UDS_NO_INDEXSESSION;
  }

  UdsGridConfig gridConfig;
  int result = udsInitializeGridConfig(&gridConfig);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = addGridServer(gridConfig, NULL, NULL, name);
  if (result != UDS_SUCCESS) {
    udsFreeGridConfig(gridConfig);
    return result;
  }

  result = makeIndexSession(gridConfig, loadType, userConfig, &session->id);
  udsFreeGridConfig(gridConfig);
  return result;
}

/**********************************************************************/
int udsCreateLocalIndex(const char       *name,
                        UdsConfiguration  userConfig,
                        UdsIndexSession  *session)
{
  if (userConfig == NULL) {
    return logErrorWithStringError(UDS_CONF_REQUIRED,
                                   "received an invalid config");
  }
  return makeLocalIndex(name, LOAD_CREATE, userConfig, session);
}

/**********************************************************************/
int udsLoadLocalIndex(const char               *name,
                      UdsIndexSession          *session)
{
  return makeLocalIndex(name, LOAD_LOAD, NULL, session);
}

/**********************************************************************/
int udsRebuildLocalIndex(const char            *name,
                         UdsIndexSession       *session)
{
  return makeLocalIndex(name, LOAD_REBUILD, NULL, session);
}

#if GRID
/**********************************************************************/
int udsAttachGridIndex(UdsGridConfig gridConfig, UdsIndexSession *session)
{
  if (session == NULL) {
    return UDS_NO_INDEXSESSION;
  }
  return makeIndexSession(gridConfig, LOAD_ATTACH, NULL, &session->id);
}
#endif /* GRID */

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
