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
 * $Id: //eng/uds-releases/gloria/src/uds/indexStateData.c#5 $
 */

#include "indexStateData.h"

#include "buffer.h"
#include "errors.h"
#include "index.h"
#include "logger.h"
#include "permassert.h"
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

static int readIndexStateData(ReadPortal *portal);
static int writeIndexStateData(IndexComponent *component,
                               BufferedWriter *writer,
                               unsigned int    zone);

/* The state file component */
const IndexComponentInfo INDEX_STATE_INFO = {
  .kind        = RL_KIND_INDEX_STATE,
  .name        = "index state",
  .fileName    = "index_state",
  .saveOnly    = false,
  .chapterSync = true,
  .multiZone   = false,
  .loader      = readIndexStateData,
  .saver       = writeIndexStateData,
  .incremental = NULL,
};

/**********************************************************************/
__attribute__((warn_unused_result))
static int decodeIndexStateData(Buffer *buffer, IndexStateData301 *state)
{
  int result = getUInt64LEFromBuffer(buffer, &state->newestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &state->oldestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &state->lastCheckpoint);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &state->unused);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (state->unused != 0) {
    return UDS_CORRUPT_COMPONENT;
  }
  result = getUInt32LEFromBuffer(buffer, &state->padding);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (state->padding != 0) {
    return UDS_CORRUPT_COMPONENT;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == 0,
                           "%zu bytes decoded of %zu expected",
                           bufferLength(buffer) - contentLength(buffer),
                           bufferLength(buffer));
  if (result != UDS_SUCCESS) {
    return UDS_CORRUPT_COMPONENT;
  }
  return result;
}

/**
 * The index state index component reader.
 *
 * @param portal the ReadPortal that handles the read of the component
 *
 * @return UDS_SUCCESS or an error code
 **/
static int readIndexStateData(ReadPortal *portal)
{
  BufferedReader *reader = NULL;
  int result = getBufferedReaderForPortal(portal, 0, &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  byte versionBuffer[sizeof(IndexStateVersion)];
  result = readFromBufferedReader(reader, versionBuffer, sizeof(versionBuffer));
  if (result != UDS_SUCCESS) {
    return result;
  }

  IndexStateVersion fileVersion;
  size_t offset = 0;
  decodeInt32LE(versionBuffer, &offset, &fileVersion.signature);
  decodeInt32LE(versionBuffer, &offset, &fileVersion.versionID);
  if (fileVersion.signature != -1 || fileVersion.versionID != 301) {
    return logErrorWithStringError(UDS_UNSUPPORTED_VERSION,
                                   "Index state version %d,%d is unsupported",
                                   fileVersion.signature,
                                   fileVersion.versionID);
  }

  Buffer *buffer;
  IndexStateData301 state;
  result = makeBuffer(sizeof(state), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = readFromBufferedReader(reader, getBufferContents(buffer),
                                  bufferLength(buffer));
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = resetBufferEnd(buffer, bufferLength(buffer));
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = decodeIndexStateData(buffer, &state);
  freeBuffer(&buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  Index *index = componentDataForPortal(portal);
  index->newestVirtualChapter = state.newestChapter;
  index->oldestVirtualChapter = state.oldestChapter;
  index->lastCheckpoint       = state.lastCheckpoint;
  return UDS_SUCCESS;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int encodeIndexStateData(Buffer *buffer, IndexStateData301 *state)
{
  int result = putUInt64LEIntoBuffer(buffer, state->newestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, state->oldestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, state->lastCheckpoint);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, state->unused);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, state->padding);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == sizeof(*state),
                           "%zu bytes encoded, of %zu expected",
                           contentLength(buffer), sizeof(state));
  return result;
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
                               BufferedWriter *writer,
                               unsigned int zone)
{
  int result = ASSERT((zone == 0), "unimplemented zone %d", zone);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t versionSize = sizeof(IndexStateVersion);
  Buffer *buffer;
  result = makeBuffer(versionSize, &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result
    = putUInt32LEIntoBuffer(buffer, INDEX_STATE_VERSION_301.signature);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result
    = putUInt32LEIntoBuffer(buffer, INDEX_STATE_VERSION_301.versionID);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == versionSize,
                           "%zu bytes encoded, of %zu expected",
                           contentLength(buffer), versionSize);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = writeToBufferedWriter(writer, getBufferContents(buffer),
                                 contentLength(buffer));
  freeBuffer(&buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  Index *index = indexComponentData(component);
  IndexStateData301 state = {
    .newestChapter  = index->newestVirtualChapter,
    .oldestChapter  = index->oldestVirtualChapter,
    .lastCheckpoint = index->lastCheckpoint,
  };
  result = makeBuffer(sizeof(IndexStateData301), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = encodeIndexStateData(buffer, &state);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = writeToBufferedWriter(writer, getBufferContents(buffer),
                                 contentLength(buffer));
  freeBuffer(&buffer);
  return result;
}
