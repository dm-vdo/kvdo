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
 * $Id: //eng/uds-releases/gloria/src/uds/masterIndexOps.c#1 $
 */
#include "masterIndexOps.h"

#include "compiler.h"
#include "errors.h"
#include "indexComponent.h"
#include "logger.h"
#include "masterIndex005.h"
#include "masterIndex006.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"
#include "zone.h"

/**********************************************************************/
static INLINE bool usesSparse(const Configuration *config)
{
  return config->geometry->sparseChaptersPerVolume > 0;
}

/**********************************************************************/
void getMasterIndexCombinedStats(const MasterIndex *masterIndex,
                                 MasterIndexStats *stats)
{
  MasterIndexStats dense, sparse;
  getMasterIndexStats(masterIndex, &dense, &sparse);
  stats->memoryAllocated = dense.memoryAllocated + sparse.memoryAllocated;
  stats->rebalanceTime   = dense.rebalanceTime   + sparse.rebalanceTime;
  stats->rebalanceCount  = dense.rebalanceCount  + sparse.rebalanceCount;
  stats->recordCount     = dense.recordCount     + sparse.recordCount;
  stats->collisionCount  = dense.collisionCount  + sparse.collisionCount;
  stats->discardCount    = dense.discardCount    + sparse.discardCount;
  stats->overflowCount   = dense.overflowCount   + sparse.overflowCount;
  stats->numLists        = dense.numLists        + sparse.numLists;
  stats->earlyFlushes    = dense.earlyFlushes    + sparse.earlyFlushes;
}

/**********************************************************************/
int makeMasterIndex(const Configuration  *config, unsigned int numZones,
                    uint64_t volumeNonce, MasterIndex **masterIndex)
{
  if (usesSparse(config)) {
    return makeMasterIndex006(config, numZones, volumeNonce, masterIndex);
  } else {
    return makeMasterIndex005(config, numZones, volumeNonce, masterIndex);
  }
}

/**********************************************************************/
int computeMasterIndexSaveBlocks(const Configuration *config,
                                 size_t blockSize, uint64_t *blockCount)
{
  size_t numBytes;
  int result = (usesSparse(config)
                ? computeMasterIndexSaveBytes006(config, &numBytes)
                : computeMasterIndexSaveBytes005(config, &numBytes));
  if (result != UDS_SUCCESS) {
    return result;
  }
  numBytes += sizeof(DeltaListSaveInfo);
  *blockCount = (numBytes + blockSize - 1) / blockSize + MAX_ZONES;
  return UDS_SUCCESS;
}

/**********************************************************************/
static int readMasterIndex(ReadPortal *portal)
{
  unsigned int numZones = countPartsForPortal(portal);
  MasterIndex *masterIndex = componentContextForPortal(portal);
  BufferedReader *readers[numZones];
  for (unsigned int z = 0; z < numZones; ++z) {
    int result = getBufferedReaderForPortal(portal, z, &readers[z]);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(result,
                                     "cannot read component for zone %u", z);
    }
  }
  return restoreMasterIndex(readers, numZones, masterIndex);
}

/**********************************************************************/
static int writeMasterIndex(IndexComponent           *component,
                            BufferedWriter           *writer,
                            unsigned int              zone,
                            IncrementalWriterCommand  command,
                            bool                     *completed)
{
  MasterIndex *masterIndex = indexComponentContext(component);
  bool isComplete = false;

  int result = UDS_SUCCESS;

  switch (command) {
    case IWC_START:
      result = startSavingMasterIndex(masterIndex, zone, writer);
      isComplete = result != UDS_SUCCESS;
      break;
    case IWC_CONTINUE:
      isComplete = isSavingMasterIndexDone(masterIndex, zone);
      break;
    case IWC_FINISH:
      result = finishSavingMasterIndex(masterIndex, zone);
      if (result == UDS_SUCCESS) {
        result = writeGuardDeltaList(writer);
      }
      isComplete = true;
      break;
    case IWC_ABORT:
      result = abortSavingMasterIndex(masterIndex, zone);
      isComplete = true;
      break;
    default:
      result = logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                         "Invalid writer command");
      break;
  }
  if (completed != NULL) {
    *completed = isComplete;
  }
  return result;
}

/**********************************************************************/

static const IndexComponentInfo MASTER_INDEX_INFO_DATA = {
  .kind        = RL_KIND_MASTER_INDEX,
  .name        = "master index",
  .fileName    = "master_index",
  .saveOnly    = false,
  .chapterSync = false,
  .multiZone   = true,
  .loader      = readMasterIndex,
  .saver       = NULL,
  .incremental = writeMasterIndex,
};
const IndexComponentInfo *const MASTER_INDEX_INFO = &MASTER_INDEX_INFO_DATA;

/**********************************************************************/
static int restoreMasterIndexBody(BufferedReader **bufferedReaders,
                                  unsigned int     numReaders,
                                  MasterIndex     *masterIndex,
                                  byte dlData[DELTA_LIST_MAX_BYTE_COUNT])
{
  // Start by reading the "header" section of the stream
  int result = startRestoringMasterIndex(masterIndex, bufferedReaders,
                                         numReaders);
  if (result != UDS_SUCCESS) {
    return result;
  }
  // Loop to read the delta lists, stopping when they have all been processed.
  for (unsigned int z = 0; z < numReaders; z++) {
    for (;;) {
      DeltaListSaveInfo dlsi;
      result = readSavedDeltaList(&dlsi, dlData, bufferedReaders[z]);
      if (result == UDS_END_OF_FILE) {
        break;
      } else if (result != UDS_SUCCESS) {
        abortRestoringMasterIndex(masterIndex);
        return result;
      }
      result = restoreDeltaListToMasterIndex(masterIndex, &dlsi, dlData);
      if (result != UDS_SUCCESS) {
        abortRestoringMasterIndex(masterIndex);
        return result;
      }
    }
  }
  if (!isRestoringMasterIndexDone(masterIndex)) {
    abortRestoringMasterIndex(masterIndex);
    return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                     "incomplete delta list data");
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int restoreMasterIndex(BufferedReader **bufferedReaders,
                       unsigned int     numReaders,
                       MasterIndex     *masterIndex)
{
  byte *dlData;
  int result = ALLOCATE(DELTA_LIST_MAX_BYTE_COUNT, byte, __func__, &dlData);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = restoreMasterIndexBody(bufferedReaders, numReaders, masterIndex,
                                  dlData);
  FREE(dlData);
  return result;
}
