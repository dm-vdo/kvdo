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
 * $Id: //eng/uds-releases/jasper/src/uds/indexStateData.c#3 $
 */

#include "indexStateData.h"

#include "buffer.h"
#include "errors.h"
#include "index.h"
#include "logger.h"
#include "uds.h"

/* The index state version header */
typedef struct {
  int32_t signature;
  int32_t versionID;
} IndexStateVersion;

/* The version 301 index state */
typedef struct {
  uint64_t newestChapter;
  uint64_t oldestChapter;
  uint64_t lastCheckpoint;
  uint32_t unused;
  uint32_t padding;
} IndexStateData301;

static const IndexStateVersion INDEX_STATE_VERSION_301 = {
  .signature = -1,
  .versionID = 301,
};

/**
 * The index state index component reader.
 *
 * @param portal the ReadPortal that handles the read of the component
 *
 * @return UDS_SUCCESS or an error code
 **/
static int readIndexStateData(ReadPortal *portal)
{
  Buffer *buffer = getStateIndexStateBuffer(portal->component->state, IO_READ);
  int result = rewindBuffer(buffer, uncompactedAmount(buffer));
  if (result != UDS_SUCCESS) {
    return result;
  }

  IndexStateVersion fileVersion;
  result = getInt32LEFromBuffer(buffer, &fileVersion.signature);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getInt32LEFromBuffer(buffer, &fileVersion.versionID);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (fileVersion.signature != -1 || fileVersion.versionID != 301) {
    return logErrorWithStringError(UDS_UNSUPPORTED_VERSION,
                                   "Index state version %d,%d is unsupported",
                                   fileVersion.signature,
                                   fileVersion.versionID);
  }

  IndexStateData301 state;
  result = getUInt64LEFromBuffer(buffer, &state.newestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &state.oldestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &state.lastCheckpoint);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &state.unused);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &state.padding);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if ((state.unused != 0) || (state.padding != 0)) {
    return UDS_CORRUPT_COMPONENT;
  }

  Index *index = indexComponentData(portal->component);
  index->newestVirtualChapter = state.newestChapter;
  index->oldestVirtualChapter = state.oldestChapter;
  index->lastCheckpoint       = state.lastCheckpoint;
  return UDS_SUCCESS;
}

/**
 * The index state index component writer.
 *
 * @param component The component whose state is to be saved (an Index)
 * @param writer    The buffered writer.
 * @param zone      The zone to write.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int writeIndexStateData(IndexComponent *component,
                               BufferedWriter *writer __attribute__((unused)),
                               unsigned int zone __attribute__((unused)))
{
  Buffer *buffer = getStateIndexStateBuffer(component->state, IO_WRITE);
  int result = resetBufferEnd(buffer, 0);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, INDEX_STATE_VERSION_301.signature);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, INDEX_STATE_VERSION_301.versionID);
  if (result != UDS_SUCCESS) {
    return result;
  }

  Index *index = indexComponentData(component);
  IndexStateData301 state = {
    .newestChapter  = index->newestVirtualChapter,
    .oldestChapter  = index->oldestVirtualChapter,
    .lastCheckpoint = index->lastCheckpoint,
  };

  result = putUInt64LEIntoBuffer(buffer, state.newestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, state.oldestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, state.lastCheckpoint);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, state.unused);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, state.padding);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/

const IndexComponentInfo INDEX_STATE_INFO = {
  .kind        = RL_KIND_INDEX_STATE,
  .name        = "index state",
  .saveOnly    = false,
  .chapterSync = true,
  .multiZone   = false,
  .ioStorage   = false,
  .loader      = readIndexStateData,
  .saver       = writeIndexStateData,
  .incremental = NULL,
};
