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
 * $Id: //eng/uds-releases/flanders/kernelLinux/uds/udsModule.c#4 $
 */

#include <linux/module.h>

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "sysfs.h"
#include "timeUtils.h"
#include "uds.h"
#include "uds-block.h"
#include "uds-param.h"

/**********************************************************************/
static int __init dedupeInit(void)
{
  memoryInit();
  logInfo("%s starting", THIS_MODULE->name);
  initSysfs();
  return 0;
}

/**********************************************************************/
static void __exit dedupeExit(void)
{
  udsShutdown();
  putSysfs();
  memoryExit();
  logInfo("%s exiting", THIS_MODULE->name);
}

/**********************************************************************/
module_init(dedupeInit);
module_exit(dedupeExit);

EXPORT_SYMBOL(UDS_MEMORY_CONFIG_256MB);
EXPORT_SYMBOL(UDS_MEMORY_CONFIG_512MB);
EXPORT_SYMBOL(UDS_MEMORY_CONFIG_768MB);
EXPORT_SYMBOL(UDS_MEMORY_CONFIG_MAX);
EXPORT_SYMBOL(udsCalculateSHA256ChunkName);
EXPORT_SYMBOL(udsCalculateMurmur3ChunkName);
EXPORT_SYMBOL(udsEqualChunkName);
EXPORT_SYMBOL(udsInitializeConfiguration);
EXPORT_SYMBOL(udsComputeIndexSize);
EXPORT_SYMBOL(udsConfigurationSetSparse);
EXPORT_SYMBOL(udsConfigurationGetSparse);
EXPORT_SYMBOL(udsConfigurationSetCheckpointFrequency);
EXPORT_SYMBOL(udsConfigurationGetCheckpointFrequency);
EXPORT_SYMBOL(udsConfigurationGetMemory);
EXPORT_SYMBOL(udsConfigurationGetChaptersPerVolume);
EXPORT_SYMBOL(udsFreeConfiguration);
EXPORT_SYMBOL(udsGetVersion);
EXPORT_SYMBOL(udsCreateLocalIndex);
EXPORT_SYMBOL(udsLoadLocalIndex);
EXPORT_SYMBOL(udsRebuildLocalIndex);
EXPORT_SYMBOL(udsCloseIndexSession);
EXPORT_SYMBOL(udsStringError);

EXPORT_SYMBOL(udsOpenBlockContext);
EXPORT_SYMBOL(udsCloseBlockContext);
EXPORT_SYMBOL(udsFlushBlockContext);
EXPORT_SYMBOL(udsRegisterDedupeBlockCallback);
EXPORT_SYMBOL(udsSetBlockContextRequestQueueLimit);
EXPORT_SYMBOL(udsSetBlockContextHashAlgorithm);
EXPORT_SYMBOL(udsPostBlock);
EXPORT_SYMBOL(udsPostBlockName);
EXPORT_SYMBOL(udsQueryBlockName);
EXPORT_SYMBOL(udsCheckBlock);
EXPORT_SYMBOL(udsUpdateBlockMapping);
EXPORT_SYMBOL(udsDeleteBlockMapping);
EXPORT_SYMBOL(udsStartChunkOperation);
EXPORT_SYMBOL(udsGetBlockContextConfiguration);
EXPORT_SYMBOL(udsGetBlockContextIndexStats);
EXPORT_SYMBOL(udsGetBlockContextStats);
EXPORT_SYMBOL(udsResetBlockContextStats);

EXPORT_SYMBOL(UDS_PARAM_FALSE);
EXPORT_SYMBOL(UDS_PARAM_TRUE);
EXPORT_SYMBOL(udsGetParameter);
EXPORT_SYMBOL(udsIterateParameter);
EXPORT_SYMBOL(udsResetParameter);
EXPORT_SYMBOL(udsSetParameter);
EXPORT_SYMBOL(udsStringValue);
EXPORT_SYMBOL(udsUnsignedValue);

EXPORT_SYMBOL_GPL(allocateMemory);
EXPORT_SYMBOL_GPL(assertionFailed);
EXPORT_SYMBOL_GPL(assertionFailedLogOnly);
EXPORT_SYMBOL_GPL(compactBuffer);
EXPORT_SYMBOL_GPL(contentLength);
EXPORT_SYMBOL_GPL(copyBytes);
EXPORT_SYMBOL_GPL(currentTime);
EXPORT_SYMBOL_GPL(duplicateString);
EXPORT_SYMBOL_GPL(ensureAvailableSpace);
EXPORT_SYMBOL_GPL(fixedSprintf);
EXPORT_SYMBOL_GPL(freeBuffer);
EXPORT_SYMBOL_GPL(freeMemory);
EXPORT_SYMBOL_GPL(getBytesFromBuffer);
EXPORT_SYMBOL_GPL(getMemoryStats);
EXPORT_SYMBOL_GPL(makeBuffer);
EXPORT_SYMBOL_GPL(nowUsec);
EXPORT_SYMBOL_GPL(putBytes);
EXPORT_SYMBOL_GPL(reallocateMemory);
EXPORT_SYMBOL_GPL(recordBioAlloc);
EXPORT_SYMBOL_GPL(recordBioFree);
EXPORT_SYMBOL_GPL(registerAllocatingThread);
EXPORT_SYMBOL_GPL(reportMemoryUsage);
EXPORT_SYMBOL_GPL(rewindBuffer);
EXPORT_SYMBOL_GPL(skipForward);
EXPORT_SYMBOL_GPL(unregisterAllocatingThread);

/**********************************************************************/

#ifdef UDS_TESTING

#include "blockIORegion.h"
#include "bufferedIORegion.h"
#include "hashUtils.h"
#include "indexCheckpoint.h"
#include "indexInternals.h"
#include "localIndexRouter.h"
#include "openChapter.h"
#include "parameter.h"
#include "random.h"
#include "recordPage.h"
#include "requestQueue.h"
#include "resourceDefs.h"
#include "searchList.h"
#include "singleFileLayout.h"
#include "volumeInternals.h"
#include "zone.h"

EXPORT_SYMBOL_GPL(INDEX_PAGE_MAP_INFO);
EXPORT_SYMBOL_GPL(MASTER_INDEX_INFO);
EXPORT_SYMBOL_GPL(OPEN_CHAPTER_INFO);
EXPORT_SYMBOL_GPL(READ_ONLY_INDEX);
EXPORT_SYMBOL_GPL(UDS_PARALLEL_FACTOR);
EXPORT_SYMBOL_GPL(UDS_PARAMETER_TEST_PARAM);
EXPORT_SYMBOL_GPL(UDS_TIME_REQUEST_TURNAROUND);
EXPORT_SYMBOL_GPL(UDS_VOLUME_READ_THREADS);
EXPORT_SYMBOL_GPL(VOLUME_MAGIC_LENGTH);
EXPORT_SYMBOL_GPL(VOLUME_MAGIC_NUMBER);
EXPORT_SYMBOL_GPL(VOLUME_VERSION);
EXPORT_SYMBOL_GPL(VOLUME_VERSION_LENGTH);

EXPORT_SYMBOL_GPL(MurmurHash3_x64_128);
EXPORT_SYMBOL_GPL(MurmurHash3_x64_128_double);
EXPORT_SYMBOL_GPL(acquireSemaphore);
EXPORT_SYMBOL_GPL(allocSprintf);
EXPORT_SYMBOL_GPL(allocateIndex);
EXPORT_SYMBOL_GPL(appendToBuffer);
EXPORT_SYMBOL_GPL(assertPageInCache);
EXPORT_SYMBOL_GPL(attemptSemaphore);
EXPORT_SYMBOL_GPL(availableSpace);
EXPORT_SYMBOL_GPL(broadcastCond);
EXPORT_SYMBOL_GPL(closeOpenChapter);
EXPORT_SYMBOL_GPL(computeBits);
EXPORT_SYMBOL_GPL(computeDeltaIndexSaveBytes);
EXPORT_SYMBOL_GPL(computeMasterIndexSaveBlocks);
EXPORT_SYMBOL_GPL(countSessions);
EXPORT_SYMBOL_GPL(createRequest);
EXPORT_SYMBOL_GPL(createThread);
EXPORT_SYMBOL_GPL(destroyBarrier);
EXPORT_SYMBOL_GPL(destroyCond);
EXPORT_SYMBOL_GPL(destroyMutex);
EXPORT_SYMBOL_GPL(destroySemaphore);
EXPORT_SYMBOL_GPL(discardIndexStateData);
EXPORT_SYMBOL_GPL(discardLastIndexStateSave);
EXPORT_SYMBOL_GPL(dispatchIndexRequest);
EXPORT_SYMBOL_GPL(emptyOpenChapterIndex);
EXPORT_SYMBOL_GPL(encodeRecordPage);
EXPORT_SYMBOL_GPL(enqueuePageRead);
EXPORT_SYMBOL_GPL(enqueueRead);
EXPORT_SYMBOL_GPL(enterBarrier);
EXPORT_SYMBOL_GPL(eventCountBroadcast);
EXPORT_SYMBOL_GPL(eventCountCancel);
EXPORT_SYMBOL_GPL(eventCountPrepare);
EXPORT_SYMBOL_GPL(eventCountWait);
EXPORT_SYMBOL_GPL(exitThread);
EXPORT_SYMBOL_GPL(extendDeltaMemory);
EXPORT_SYMBOL_GPL(fillRandomly);
EXPORT_SYMBOL_GPL(findIndexComponent);
EXPORT_SYMBOL_GPL(findInvalidateAndMakeLeastRecent);
EXPORT_SYMBOL_GPL(findVolumeChapterBoundariesImpl);
EXPORT_SYMBOL_GPL(finishCheckpointing);
EXPORT_SYMBOL_GPL(finishPreviousChapter);
EXPORT_SYMBOL_GPL(finishSavingDeltaIndex);
EXPORT_SYMBOL_GPL(finishSession);
EXPORT_SYMBOL_GPL(flushBufferedWriter);
EXPORT_SYMBOL_GPL(formatVolume);
EXPORT_SYMBOL_GPL(freeBufferedReader);
EXPORT_SYMBOL_GPL(freeBufferedWriter);
EXPORT_SYMBOL_GPL(freeConfiguration);
EXPORT_SYMBOL_GPL(freeEventCount);
EXPORT_SYMBOL_GPL(freeFunnelQueue);
EXPORT_SYMBOL_GPL(freeGeometry);
EXPORT_SYMBOL_GPL(freeIndex);
EXPORT_SYMBOL_GPL(freeIndexRouter);
EXPORT_SYMBOL_GPL(freeOpenChapter);
EXPORT_SYMBOL_GPL(freeOpenChapterIndex);
EXPORT_SYMBOL_GPL(freePageCache);
EXPORT_SYMBOL_GPL(freeRadixSorter);
EXPORT_SYMBOL_GPL(freeRequest);
EXPORT_SYMBOL_GPL(freeSearchList);
EXPORT_SYMBOL_GPL(freeThreadStatistics);
EXPORT_SYMBOL_GPL(freeVolume);
EXPORT_SYMBOL_GPL(funnelQueuePoll);
EXPORT_SYMBOL_GPL(futureTime);
EXPORT_SYMBOL_GPL(generateChunkName);
EXPORT_SYMBOL_GPL(getBaseContext);
EXPORT_SYMBOL_GPL(getBufferContents);
EXPORT_SYMBOL_GPL(getBufferedReaderPosition);
EXPORT_SYMBOL_GPL(getBytes);
EXPORT_SYMBOL_GPL(getChapterIndexVirtualChapterNumber);
EXPORT_SYMBOL_GPL(getDeltaIndexDlistBitsUsed);
EXPORT_SYMBOL_GPL(getDeltaIndexEntry);
EXPORT_SYMBOL_GPL(getDeltaIndexHighestListNumber);
EXPORT_SYMBOL_GPL(getDeltaIndexLowestListNumber);
EXPORT_SYMBOL_GPL(getDeltaIndexStats);
EXPORT_SYMBOL_GPL(getMasterIndexCombinedStats);
EXPORT_SYMBOL_GPL(getNumCores);
EXPORT_SYMBOL_GPL(getOpenChapterIndexSize);
EXPORT_SYMBOL_GPL(getPage);
EXPORT_SYMBOL_GPL(getPageCacheCounters);
EXPORT_SYMBOL_GPL(getPageFromCache);
EXPORT_SYMBOL_GPL(getPageLocked);
EXPORT_SYMBOL_GPL(getPageProtected);
EXPORT_SYMBOL_GPL(getSession);
EXPORT_SYMBOL_GPL(getSessionContents);
EXPORT_SYMBOL_GPL(getSparseCacheCounters);
EXPORT_SYMBOL_GPL(getThreadId);
EXPORT_SYMBOL_GPL(getThreadStatistics);
EXPORT_SYMBOL_GPL(getZoneCount);
EXPORT_SYMBOL_GPL(hasSparseChapters);
EXPORT_SYMBOL_GPL(initCond);
EXPORT_SYMBOL_GPL(initMutex);
EXPORT_SYMBOL_GPL(initializeBarrier);
EXPORT_SYMBOL_GPL(initializeChapterIndexPage);
EXPORT_SYMBOL_GPL(initializeDeltaIndex);
EXPORT_SYMBOL_GPL(initializeDeltaMemory);
EXPORT_SYMBOL_GPL(initializeDeltaMemoryPage);
EXPORT_SYMBOL_GPL(initializeSemaphore);
EXPORT_SYMBOL_GPL(initializeSession);
EXPORT_SYMBOL_GPL(invalidatePageCacheForChapter);
EXPORT_SYMBOL_GPL(invalidateSparseCache);
EXPORT_SYMBOL_GPL(isChapterSparse);
EXPORT_SYMBOL_GPL(isRestoringDeltaIndexDone);
EXPORT_SYMBOL_GPL(joinThreads);
EXPORT_SYMBOL_GPL(loadOpenChapters);
EXPORT_SYMBOL_GPL(lockMutex);
EXPORT_SYMBOL_GPL(logDebug);
EXPORT_SYMBOL_GPL(logError);
EXPORT_SYMBOL_GPL(logErrorWithStringError);
EXPORT_SYMBOL_GPL(logInfo);
EXPORT_SYMBOL_GPL(makeBufferedReader);
EXPORT_SYMBOL_GPL(makeBufferedRegion);
EXPORT_SYMBOL_GPL(makeBufferedWriter);
EXPORT_SYMBOL_GPL(makeConfiguration);
EXPORT_SYMBOL_GPL(makeEventCount);
EXPORT_SYMBOL_GPL(makeFunnelQueue);
EXPORT_SYMBOL_GPL(makeGeometry);
EXPORT_SYMBOL_GPL(makeIndex);
EXPORT_SYMBOL_GPL(makeIndexLayout);
EXPORT_SYMBOL_GPL(makeLocalIndexRouter);
EXPORT_SYMBOL_GPL(makeMasterIndex);
EXPORT_SYMBOL_GPL(makeOpenChapter);
EXPORT_SYMBOL_GPL(makeOpenChapterIndex);
EXPORT_SYMBOL_GPL(makePageCache);
EXPORT_SYMBOL_GPL(makePageMostRecent);
EXPORT_SYMBOL_GPL(makeRadixSorter);
EXPORT_SYMBOL_GPL(makeRequestQueue);
EXPORT_SYMBOL_GPL(makeSearchList);
EXPORT_SYMBOL_GPL(makeSessionGroup);
EXPORT_SYMBOL_GPL(makeUnrecoverable);
EXPORT_SYMBOL_GPL(makeVolume);
EXPORT_SYMBOL_GPL(mapToPhysicalPage);
EXPORT_SYMBOL_GPL(memdup);
EXPORT_SYMBOL_GPL(minMasterIndexDeltaLists);
EXPORT_SYMBOL_GPL(moveBits);
EXPORT_SYMBOL_GPL(nextDeltaIndexEntry);
EXPORT_SYMBOL_GPL(nextToken);
EXPORT_SYMBOL_GPL(openBlockRegion);
EXPORT_SYMBOL_GPL(openChapterSize);
EXPORT_SYMBOL_GPL(packOpenChapterIndexPage);
EXPORT_SYMBOL_GPL(printThreadStatistics);
EXPORT_SYMBOL_GPL(purgeSearchList);
EXPORT_SYMBOL_GPL(putDeltaIndexEntry);
EXPORT_SYMBOL_GPL(putMasterIndexRecord);
EXPORT_SYMBOL_GPL(putOpenChapter);
EXPORT_SYMBOL_GPL(putOpenChapterIndexRecord);
EXPORT_SYMBOL_GPL(putPageInCache);
EXPORT_SYMBOL_GPL(radixSort);
EXPORT_SYMBOL_GPL(randomInRange);
EXPORT_SYMBOL_GPL(readBufferedData);
EXPORT_SYMBOL_GPL(readFromBufferedReader);
EXPORT_SYMBOL_GPL(readSavedDeltaList);
EXPORT_SYMBOL_GPL(registerErrorBlock);
EXPORT_SYMBOL_GPL(relTimeToString);
EXPORT_SYMBOL_GPL(releaseBaseContext);
EXPORT_SYMBOL_GPL(releaseSemaphore);
EXPORT_SYMBOL_GPL(releaseSession);
EXPORT_SYMBOL_GPL(removeDeltaIndexEntry);
EXPORT_SYMBOL_GPL(removeFromOpenChapter);
EXPORT_SYMBOL_GPL(removeMasterIndexRecord);
EXPORT_SYMBOL_GPL(requestQueueEnqueue);
EXPORT_SYMBOL_GPL(requestQueueFinish);
EXPORT_SYMBOL_GPL(resetOpenChapter);
EXPORT_SYMBOL_GPL(resetZoneCount);
EXPORT_SYMBOL_GPL(restoreDeltaListToDeltaIndex);
EXPORT_SYMBOL_GPL(restoreMasterIndex);
EXPORT_SYMBOL_GPL(sameBits);
EXPORT_SYMBOL_GPL(sansUnrecoverable);
EXPORT_SYMBOL_GPL(saveIndex);
EXPORT_SYMBOL_GPL(saveOpenChapters);
EXPORT_SYMBOL_GPL(searchChapterIndexPage);
EXPORT_SYMBOL_GPL(searchOpenChapter);
EXPORT_SYMBOL_GPL(searchRecordPage);
EXPORT_SYMBOL_GPL(selectVictimInCache);
EXPORT_SYMBOL_GPL(setBufferedReaderPosition);
EXPORT_SYMBOL_GPL(setBytes);
EXPORT_SYMBOL_GPL(setDeltaEntryValue);
EXPORT_SYMBOL_GPL(setIndexCheckpointFrequency);
EXPORT_SYMBOL_GPL(setMasterIndexRecordChapter);
EXPORT_SYMBOL_GPL(setRequestRestarter);
EXPORT_SYMBOL_GPL(setTestParameterDefinitionFunc);
EXPORT_SYMBOL_GPL(setZoneCount);
EXPORT_SYMBOL_GPL(shutdownSessionGroup);
EXPORT_SYMBOL_GPL(signalCond);
EXPORT_SYMBOL_GPL(sleepFor);
EXPORT_SYMBOL_GPL(spaceRemainingInWriteBuffer);
EXPORT_SYMBOL_GPL(startDeltaIndexSearch);
EXPORT_SYMBOL_GPL(startRestoringDeltaIndex);
EXPORT_SYMBOL_GPL(startSavingDeltaIndex);
EXPORT_SYMBOL_GPL(stopChapterWriter);
EXPORT_SYMBOL_GPL(stringError);
EXPORT_SYMBOL_GPL(stringErrorName);
EXPORT_SYMBOL_GPL(timeDifference);
EXPORT_SYMBOL_GPL(timedWaitCond);
EXPORT_SYMBOL_GPL(udsShutdown);
EXPORT_SYMBOL_GPL(uninitializeDeltaIndex);
EXPORT_SYMBOL_GPL(uninitializeDeltaMemory);
EXPORT_SYMBOL_GPL(unlockMutex);
EXPORT_SYMBOL_GPL(updateVolumeSize);
EXPORT_SYMBOL_GPL(vAppendToBuffer);
EXPORT_SYMBOL_GPL(vLogMessage);
EXPORT_SYMBOL_GPL(validateDeltaIndex);
EXPORT_SYMBOL_GPL(validateDeltaLists);
EXPORT_SYMBOL_GPL(validateNumericRange);
EXPORT_SYMBOL_GPL(waitCond);
EXPORT_SYMBOL_GPL(wasBufferedWriterUsed);
EXPORT_SYMBOL_GPL(wrapBuffer);
EXPORT_SYMBOL_GPL(wrapVsnprintf);
EXPORT_SYMBOL_GPL(writeChapter);
EXPORT_SYMBOL_GPL(writeGuardDeltaList);
EXPORT_SYMBOL_GPL(writeIndexPages);
EXPORT_SYMBOL_GPL(writeRecordPages);
EXPORT_SYMBOL_GPL(writeToBufferedWriter);
EXPORT_SYMBOL_GPL(yieldScheduler);
EXPORT_SYMBOL_GPL(zeroBytes);
#endif /* UDS_TESTING */

/**********************************************************************/

MODULE_DESCRIPTION("deduplication engine");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(UDS_VERSION);
