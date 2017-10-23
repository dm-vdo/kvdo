/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/src/uds/indexStateData.c#2 $
 */

#include "indexStateData.h"

#include "errors.h"
#include "index.h"
#include "logger.h"
#include "permassert.h"
#include "uds.h"

/*
 * XXX:
 * I have no idea what the "signature" field of this structure is for, nor
 * what the scheme is supposed to be for changing the version of the file
 * in which this is used. I asked tomj, who wrote this originally, and he
 * does not remember, nor does the relevant changeset explain it.
 *
 * My best guess is that the signature field is there to ensure that a
 * 2.0 or earlier index state file can not ever be misinterpreted as a
 * newer version.
 *
 * As for updating, I'm going to make something up and say that the version id
 * is incremented by 1 each time we change it.
 *
 * --corwin
 */
typedef struct {
  int32_t signature;
  int32_t versionID;
} IndexStateVersion;

/* Function pointers for reading and writing index state files. */
typedef int (*IndexStateReader)(BufferedReader *reader, Index *index);
typedef int (*IndexStateWriter)(BufferedWriter *writer, Index *index);

/* A structure to manipulate a version of the index state file */
typedef struct {
  const IndexStateVersion *version;
  IndexStateReader         reader;
  IndexStateWriter         writer;
} IndexStateFile;

/* The version 300 index state */
typedef struct {
  uint64_t newestChapter;
  uint32_t oldestChapter;
  uint32_t lastCheckpoint;
  uint32_t id;
} IndexStateData300;

static const IndexStateVersion INDEX_STATE_VERSION_300 = {
  .signature = -1,
  .versionID = 300,
};

static int readIndexState300(BufferedReader *reader, Index *index);

static const IndexStateFile INDEX_STATE_300 = {
  .version = &INDEX_STATE_VERSION_300,
  .reader  = &readIndexState300,
  .writer  = NULL,
};

/* The version 301 index state */
typedef struct {
  uint64_t newestChapter;
  uint64_t oldestChapter;
  uint64_t lastCheckpoint;
  uint32_t id;
  uint32_t padding;
} IndexStateData301;

static const IndexStateVersion INDEX_STATE_VERSION_301 = {
  .signature = -1,
  .versionID = 301,
};

static int readIndexState301(BufferedReader *reader, Index *index);
static int writeIndexState301(BufferedWriter *writer, Index *index);

static const IndexStateFile INDEX_STATE_301 = {
  .version = &INDEX_STATE_VERSION_301,
  .reader  = &readIndexState301,
  .writer  = &writeIndexState301,
};

/**********************************************************************/
static const IndexStateFile *const SUPPORTED_STATES[] = {
  &INDEX_STATE_301,
  &INDEX_STATE_300,
  NULL
};

/* The current index state */
static const IndexStateFile *const CURRENT_INDEX_STATE = &INDEX_STATE_301;

/* The index state file component reader and writer */
static int readIndexStateData(ComponentPortal *portal);
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

/**
 * Read a version 301 index state info.
 *
 * @param reader        Where to read from.
 * @param index         The index to configure from the state info.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int readIndexState301(BufferedReader *reader, Index *index)
{
  IndexStateData301 state;
  int result = readFromBufferedReader(reader, &state, sizeof(state));
  if (result != UDS_SUCCESS) {
    return result;
  }

  index->newestVirtualChapter = state.newestChapter;
  index->oldestVirtualChapter = state.oldestChapter;
  index->lastCheckpoint       = state.lastCheckpoint;
  index->id                   = state.id;

  return UDS_SUCCESS;
}

/**
 * Write a version 301 index state info.
 *
 * @param writer        Where to write to.
 * @param index         The index whose state is to be saved.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int writeIndexState301(BufferedWriter *writer, Index *index)
{
  IndexStateData301 state = {
    .newestChapter  = index->newestVirtualChapter,
    .oldestChapter  = index->oldestVirtualChapter,
    .lastCheckpoint = index->lastCheckpoint,
    .id             = index->id,
  };
  return writeToBufferedWriter(writer, &state, sizeof(IndexStateData301));
}

/**
 * Read a version 300 index state info.
 *
 * @param reader        Where to read from.
 * @param index         The index to configure from the state info.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int readIndexState300(BufferedReader *reader, Index *index)
{
  IndexStateData300 state;
  int result = readFromBufferedReader(reader, &state, sizeof(state));
  if (result != UDS_SUCCESS) {
    return result;
  }

  Geometry *geometry = index->volume->geometry;

  index->newestVirtualChapter = state.newestChapter;
  index->oldestVirtualChapter = mapToVirtualChapterNumber(geometry,
                                                          state.newestChapter,
                                                          state.oldestChapter);
  index->lastCheckpoint       = mapToVirtualChapterNumber(geometry,
                                                          state.newestChapter,
                                                          state.lastCheckpoint);
  index->id                   = mapToVirtualChapterNumber(geometry,
                                                          state.newestChapter,
                                                          state.id);

  return UDS_SUCCESS;
}

/**
 * The index state index component reader.
 *
 * @param reader        Where to read from.
 * @param component     The component to configure from the info (an Index).
 *
 * @return UDS_SUCCESS or an error code
 **/
static int readIndexStateData(ComponentPortal *portal)
{
  BufferedReader *reader = NULL;
  int result = getBufferedReader(portal, 0, &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  IndexStateVersion fileVersion;
  result = readFromBufferedReader(reader, &fileVersion,
                                  sizeof(IndexStateVersion));
  if (result != UDS_SUCCESS) {
    return result;
  }

  Index *index = componentDataForPortal(portal);

  if (fileVersion.signature == -1) {
    for (unsigned int i = 0; SUPPORTED_STATES[i] != NULL; i++) {
      if (fileVersion.versionID == SUPPORTED_STATES[i]->version->versionID) {
        return SUPPORTED_STATES[i]->reader(reader, index);
      }
    }
  }

  return logErrorWithStringError(UDS_UNSUPPORTED_VERSION,
                                 "Index state version %d,%d is unsupported",
                                 fileVersion.signature,
                                 fileVersion.versionID);
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

  result = writeToBufferedWriter(writer, CURRENT_INDEX_STATE->version,
                                 sizeof(IndexStateVersion));
  if (result != UDS_SUCCESS) {
    return result;
  }

  Index *index = indexComponentData(component);
  return CURRENT_INDEX_STATE->writer(writer, index);
}
