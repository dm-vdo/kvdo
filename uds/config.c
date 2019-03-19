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
 * $Id: //eng/uds-releases/gloria/src/uds/config.c#1 $
 */

#include "config.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

/**********************************************************************/
int udsInitializeGridConfig(UdsGridConfig *gridConfig)
{
  if (gridConfig == NULL) {
    return logErrorWithStringError(UDS_CONF_PTR_REQUIRED,
                                   "received a NULL config pointer");
  }
  UdsGridConfig config;
  int result = ALLOCATE(1, struct udsGridConfig, "UdsGridConfig",
                        &config);
  if (result == UDS_SUCCESS) {
    *gridConfig = config;
  }
  return result;
}

/**********************************************************************/
void freeIndexLocation(IndexLocation *loc)
{
  if (loc == NULL) {
    return;
  }

  FREE(loc->host);
  FREE(loc->port);
  FREE(loc->directory);
}

/**********************************************************************/
void udsFreeGridConfig(UdsGridConfig gridConfig)
{
  if (gridConfig == NULL) {
    return;
  }
  for (unsigned int i = 0; i < gridConfig->numLocations; i++) {
    freeIndexLocation(&gridConfig->locations[i]);
  }
  FREE(gridConfig->locations);
  FREE(gridConfig);
}

/**********************************************************************/
static int copyParameter(IndexLocation *location, const char *src, char **dest)
{
  if (src == NULL) {
    *dest = NULL;
    return UDS_SUCCESS;
  }

  int result = duplicateString(src, "location paramter", dest);
  if (result != UDS_SUCCESS) {
    freeIndexLocation(location);
  }
  return result;
}

/**********************************************************************/
int addGridServer(UdsGridConfig  gridConfig,
                  const char    *host,
                  const char    *port,
                  const char    *directory)
{
  if (gridConfig->numLocations == gridConfig->maxLocations) {
    IndexLocation *newLocations;

    size_t newMax = gridConfig->maxLocations + 8;

    int result = ALLOCATE(newMax, IndexLocation, "grid locations",
                          &newLocations);

    if (result != UDS_SUCCESS) {
      return result;
    }

    gridConfig->maxLocations = newMax;

    for (unsigned int i = 0; i < gridConfig->numLocations; i++) {
      newLocations[i] = gridConfig->locations[i];
    }

    FREE(gridConfig->locations);
    gridConfig->locations = newLocations;
  }

  IndexLocation *location = &gridConfig->locations[gridConfig->numLocations];
  location->host = location->port = location->directory = NULL;

  int result = copyParameter(location, host, &location->host);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = copyParameter(location, port, &location->port);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = copyParameter(location, directory, &location->directory);
  if (result != UDS_SUCCESS) {
    return result;
  }

  gridConfig->numLocations++;
  return UDS_SUCCESS;
}

/**********************************************************************/
int udsAddGridServer(UdsGridConfig  gridConfig,
                     const char    *host,
                     const char    *port)
{
  if (gridConfig == NULL) {
    return logErrorWithStringError(UDS_CONF_REQUIRED,
                                   "received an invalid config");
  }
  return addGridServer(gridConfig, host, port, NULL);
}

/**********************************************************************/
int printIndexLocation(const IndexLocation  *indexLocation,
                       char                **output)
{
  bool  gotHost = (indexLocation->host != NULL);
  bool  gotPort = (indexLocation->port != NULL);
  bool  gotDir  = (indexLocation->directory != NULL);
  char *str     = NULL;

  int result = allocSprintf(__func__, &str, "%s%s%s%s%s",
                            gotHost ? indexLocation->host : "",
                            gotHost ? ":" : "",
                            gotPort ? indexLocation->port : "",
                            (gotHost && gotDir) ? ":" : "",
                            gotDir ? indexLocation->directory : "");
  if (result != UDS_SUCCESS) {
    return result;
  }

  *output = str;
  return UDS_SUCCESS;
}

/**********************************************************************/
static int gridConfigToString(UdsGridConfig gridConfig, char **output)
{
  if (gridConfig->numLocations == 0) {
    char *str;
    int result = duplicateString("<none>", "printGridConfig(): no locations",
                                 &str);
    if (result != UDS_SUCCESS) {
      return result;
    }
    *output = str;
    return UDS_SUCCESS;
  }

  if (gridConfig->numLocations == 1) {
    /* only one location, print it inline */
    return printIndexLocation(gridConfig->locations, output);
  }

  /*
   * otherwise, print it in the following format:
   *   location1
   *   location2
   *   ...
   */
  char *tmpStr = NULL;
  char *str    = NULL;
  char *locStr = NULL;

  for (unsigned int i = 0; i < gridConfig->numLocations; i++) {
    int result = printIndexLocation(gridConfig->locations + i, &locStr);
    if (result != UDS_SUCCESS) {
      FREE(str);
      return result;
    }

    if (str == NULL) {
      str = locStr;
      locStr = NULL;
    } else {
      result = allocSprintf(__func__, &tmpStr, "%s %s", str, locStr);
      FREE(str);
      FREE(locStr);
      if (result != UDS_SUCCESS) {
        return result;
      }
      str = tmpStr;
      tmpStr = NULL;
    }
  }

  *output = str;
  return UDS_SUCCESS;
}

/**********************************************************************/
void logGridConfig(UdsGridConfig gridConfig, const char *message)
{
  char *gridConfigString = NULL;
  int result = gridConfigToString(gridConfig, &gridConfigString);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Failed to allocate grid config string");
    logNotice("%s", message);
  } else {
    logNotice("%s: %s", message, gridConfigString);
    FREE(gridConfigString);
  }
}

/**********************************************************************/
bool areUdsConfigurationsEqual(UdsConfiguration a, UdsConfiguration b)
{
  bool result = true;
  if (a->recordPagesPerChapter != b->recordPagesPerChapter) {
    logError("Record pages per chapter (%u) does not match (%u)",
             a->recordPagesPerChapter, b->recordPagesPerChapter);
    result = false;
  }
  if (a->chaptersPerVolume != b->chaptersPerVolume) {
    logError("Chapter count (%u) does not match (%u)",
             a->chaptersPerVolume, b->chaptersPerVolume);
    result = false;
  }
  if (a->sparseChaptersPerVolume != b->sparseChaptersPerVolume) {
    logError("Sparse chapter count (%u) does not match (%u)",
             a->sparseChaptersPerVolume, b->sparseChaptersPerVolume);
    result = false;
  }
  if (a->cacheChapters != b->cacheChapters) {
    logError("Cache size (%u) does not match (%u)",
             a->cacheChapters, b->cacheChapters);
    result = false;
  }
  if (a->checkpointFrequency != b->checkpointFrequency) {
    logError("Checkpoint frequency (%u) does not match (%u)",
             a->checkpointFrequency, b->checkpointFrequency);
    result = false;
  }
  if (a->masterIndexMeanDelta != b->masterIndexMeanDelta) {
    logError("Master index mean delta (%u) does not match (%u)",
             a->masterIndexMeanDelta, b->masterIndexMeanDelta);
    result = false;
  }
  if (a->bytesPerPage != b->bytesPerPage) {
    logError("Bytes per page value (%u) does not match (%u)",
             a->bytesPerPage, b->bytesPerPage);
    result = false;
  }
  if (a->sparseSampleRate != b->sparseSampleRate) {
    logError("Sparse sample rate (%u) does not match (%u)",
             a->sparseSampleRate, b->sparseSampleRate);
    result = false;
  }
  if (a->nonce != b->nonce) {
    logError("Nonce (%" PRIu64 ") does not match (%" PRIu64 ")",
             a->nonce, b->nonce);
    result = false;
  }
  return result;
}

/**********************************************************************/
int printUdsConfiguration(UdsConfiguration conf, int indent, char **output)
{
  char *str = NULL;
  int result = allocSprintf(__func__, &str,
                            "%*sRecord pages per chapter:   %10u\n"
                            "%*sChapters per volume:        %10u\n"
                            "%*sSparse chapters per volume: %10u\n"
                            "%*sCache size (chapters):      %10u\n"
                            "%*sCheckpoint frequency:       %10u\n"
                            "%*sMaster index mean delta:    %10u\n"
                            "%*sBytes per page:             %10u\n"
                            "%*sSparse sample rate:         %10u\n"
                            "%*sNonce:                      %" PRIu64,
                            indent, "", conf->recordPagesPerChapter,
                            indent, "", conf->chaptersPerVolume,
                            indent, "", conf->sparseChaptersPerVolume,
                            indent, "", conf->cacheChapters,
                            indent, "", conf->checkpointFrequency,
                            indent, "", conf->masterIndexMeanDelta,
                            indent, "", conf->bytesPerPage,
                            indent, "", conf->sparseSampleRate,
                            indent, "", conf->nonce);
  if (result != UDS_SUCCESS) {
    return result;
  }

  *output = str;
  return UDS_SUCCESS;
}
