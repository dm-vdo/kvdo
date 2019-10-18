/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/src/uds/config.c#2 $
 */

#include "config.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

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
    logError("Nonce (%llu) does not match (%llu)",
             a->nonce, b->nonce);
    result = false;
  }
  return result;
}

/**********************************************************************/
void logUdsConfiguration(UdsConfiguration conf)
{
  logDebug("Configuration:");
  logDebug("  Record pages per chapter:   %10u", conf->recordPagesPerChapter);
  logDebug("  Chapters per volume:        %10u", conf->chaptersPerVolume);
  logDebug("  Sparse chapters per volume: %10u", conf->sparseChaptersPerVolume);
  logDebug("  Cache size (chapters):      %10u", conf->cacheChapters);
  logDebug("  Master index mean delta:    %10u", conf->masterIndexMeanDelta);
  logDebug("  Bytes per page:             %10u", conf->bytesPerPage);
  logDebug("  Sparse sample rate:         %10u", conf->sparseSampleRate);
  logDebug("  Nonce:                      %llu", conf->nonce);
}
