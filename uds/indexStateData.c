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
 * $Id: //eng/uds-releases/krusty/src/uds/indexStateData.c#7 $
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
  struct buffer *buffer
    = get_state_index_state_buffer(portal->component->state, IO_READ);
  int result = rewind_buffer(buffer, uncompacted_amount(buffer));
  if (result != UDS_SUCCESS) {
    return result;
  }

  IndexStateVersion fileVersion;
  result = get_int32_le_from_buffer(buffer, &fileVersion.signature);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_int32_le_from_buffer(buffer, &fileVersion.versionID);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (fileVersion.signature != -1 || fileVersion.versionID != 301) {
    return logErrorWithStringError(UDS_UNSUPPORTED_VERSION,
                                   "index state version %d,%d is unsupported",
                                   fileVersion.signature,
                                   fileVersion.versionID);
  }

  IndexStateData301 state;
  result = get_uint64_le_from_buffer(buffer, &state.newestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint64_le_from_buffer(buffer, &state.oldestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint64_le_from_buffer(buffer, &state.lastCheckpoint);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint32_le_from_buffer(buffer, &state.unused);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint32_le_from_buffer(buffer, &state.padding);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if ((state.unused != 0) || (state.padding != 0)) {
    return UDS_CORRUPT_COMPONENT;
  }

  struct index *index = indexComponentData(portal->component);
  index->newest_virtual_chapter = state.newestChapter;
  index->oldest_virtual_chapter = state.oldestChapter;
  index->last_checkpoint        = state.lastCheckpoint;
  return UDS_SUCCESS;
}

/**
 * The index state index component writer.
 *
 * @param component The component whose state is to be saved (an index)
 * @param writer    The buffered writer.
 * @param zone      The zone to write.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int
writeIndexStateData(IndexComponent         *component,
                    struct buffered_writer *writer  __attribute__((unused)),
                    unsigned int            zone __attribute__((unused)))
{
  struct buffer *buffer
    = get_state_index_state_buffer(component->state, IO_WRITE);
  int result = reset_buffer_end(buffer, 0);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_uint32_le_into_buffer(buffer, INDEX_STATE_VERSION_301.signature);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_uint32_le_into_buffer(buffer, INDEX_STATE_VERSION_301.versionID);
  if (result != UDS_SUCCESS) {
    return result;
  }

  struct index *index = indexComponentData(component);
  IndexStateData301 state = {
    .newestChapter  = index->newest_virtual_chapter,
    .oldestChapter  = index->oldest_virtual_chapter,
    .lastCheckpoint = index->last_checkpoint,
  };

  result = put_uint64_le_into_buffer(buffer, state.newestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_uint64_le_into_buffer(buffer, state.oldestChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_uint64_le_into_buffer(buffer, state.lastCheckpoint);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_uint32_le_into_buffer(buffer, state.unused);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_uint32_le_into_buffer(buffer, state.padding);
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
