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
 * $Id: //eng/vdo-releases/magnesium-rhel7.6/src/c++/vdo/kernel/readCache.h#1 $
 */
#ifndef READCACHE_H
#define READCACHE_H

#include "dataKVIO.h"
#include "ioSubmitter.h"
#include "kernelLayer.h"

typedef enum readBlockOperation {
  READ_NO_OPERATION = 0,
  READ_COMPRESSED_DATA,
  READ_VERIFY_DEDUPE,
} ReadBlockOperation;

/**
 * Allocate and initialize a ReadCache.
 *
 * @param [in]  layer           The associated kernel layer
 * @param [in]  numEntries      The number of read cache entries per zone
 * @param [in]  zoneCount       The number of zones to create
 * @param [out] readCachePtr    The new ReadCache
 *
 * @return success or an error code
 **/
int makeReadCache(KernelLayer   *layer,
                  unsigned int   numEntries,
                  unsigned int   zoneCount,
                  ReadCache    **readCachePtr);

/**
 * Free a ReadCache.
 *
 * @param readCachePtr    The ReadCache to free
 **/
void freeReadCache(ReadCache **readCachePtr);

/**
 * Will fetch the data for a block, either from the cache or from storage. The
 * fetched data will be uncompressed when the callback is called, at which
 * point the result of the read operation will be in the DataKVIO's ReadBlock's
 * status field, and, on success, the data will be in the ReadBlock's buffer.
 *
 * @param dataVIO       The DataVIO to read a block in for
 * @param location      The physical block number to read from
 * @param mappingState  The mapping state of the block to read
 * @param operation     The read block operation to perform
 * @param callback      The function to call when the read is done
 **/
void kvdoReadBlock(DataVIO             *dataVIO,
                   PhysicalBlockNumber  location,
                   BlockMappingState    mappingState,
                   ReadBlockOperation   operation,
                   DataKVIOCallback     callback);

/**
 * Releases a block in a read cache, possibly scheduling the work to be done in
 * a different thread associated with the specified pbn. The caller surrenders
 * use of the block at the time of the call.
 *
 * @param layer      The kernel layer
 * @param readBlock  The ReadBlock to release
 **/
void runReadCacheReleaseBlock(KernelLayer *layer, ReadBlock *readBlock);

/**
 * Returns the operational statistics of the read cache.
 *
 * @param readCache The read cache
 *
 * @return ReadCacheStats
 **/
ReadCacheStats readCacheGetStats(ReadCache *readCache);

/**
 * Dump the read cache to the log.
 *
 * @param readCache         The read cache
 * @param dumpBusyElements  True for list of busy cache entries
 * @param dumpAllElements   True for complete output
 **/
void readCacheDump(ReadCache *readCache,
                   bool       dumpBusyElements,
                   bool       dumpAllElements);

/**
 * Invalidate any read cache entries corresponding to the physical
 * address in the KVIO, and submit kvio->bio to the device. Some or
 * all of the actual work may be performed in another thread; the
 * caller surrenders control of the KVIO.
 *
 * After the I/O operation completes, the kvio->bio->bi_end_io
 * callback is invoked.
 *
 * @param kvio      The KVIO with the bio to be submitted
 * @param action    The work queue action code to prioritize processing
 **/
void invalidateCacheAndSubmitBio(KVIO *kvio, BioQAction action);

#endif /* READCACHE_H */
