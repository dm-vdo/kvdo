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
 * $Id: //eng/uds-releases/gloria/src/uds/indexZone.h#2 $
 */

#ifndef INDEX_ZONE_H
#define INDEX_ZONE_H

#include "common.h"
#include "openChapterZone.h"
#include "request.h"

typedef struct {
  struct index    *index;
  OpenChapterZone *openChapter;
  OpenChapterZone *writingChapter;
  uint64_t         oldestVirtualChapter;
  uint64_t         newestVirtualChapter;
  unsigned int     id;
} IndexZone;

/**
 * Allocate an index zone.
 *
 * @param index      The index receiving the zone
 * @param zoneNumber The number of the zone to allocate
 * @param readOnly   <code>true</code> if the index is read only
 *
 * @return UDS_SUCCESS or an error code.
 **/
int makeIndexZone(struct index *index, unsigned int zoneNumber, bool readOnly)
  __attribute__((warn_unused_result));

/**
 * Clean up an index zone.
 *
 * @param zone The index zone to free
 *
 * @return UDS_SUCCESS or an error code.
 **/
void freeIndexZone(IndexZone *zone);

/**
 * Check whether a chapter is sparse or dense based on the current state of
 * the index zone.
 *
 * @param zone            The index zone to check against
 * @param virtualChapter  The virtual chapter number of the chapter to check
 *
 * @return true if the chapter is in the sparse part of the volume
 **/
bool isZoneChapterSparse(const IndexZone *zone,
                         uint64_t         virtualChapter)
  __attribute__((warn_unused_result));

/**
 * Set the active chapter numbers for a zone based on its index. The active
 * chapters consist of the range of chapters from the current oldest to
 * the current newest virtual chapter.
 *
 * @param zone          The zone to set
 **/
void setActiveChapters(IndexZone *zone);

/**
 * Dispatch a control request to an index zone.
 *
 * @param request The request to dispatch
 *
 * @return UDS_SUCCESS or an error code
 **/
int dispatchIndexZoneControlRequest(Request *request)
  __attribute__((warn_unused_result));

/**
 * Execute a sparse chapter index cache barrier control request on the zone
 * worker thread. This call into the sparse cache to coordinate the cache
 * update with the other zones.
 *
 * @param zone     The index zone receiving the barrier message
 * @param barrier  The barrier control message data
 *
 * @return UDS_SUCCESS or an error code if the chapter index could not be
 *         read or decoded
 **/
int executeSparseCacheBarrierMessage(IndexZone          *zone,
                                     BarrierMessageData *barrier)
  __attribute__((warn_unused_result));

/**
 * Open the next chapter.
 *
 * @param zone    The zone containing the open chapter
 * @param request The request which requires the next chapter to be
 *                opened
 *
 * @return UDS_SUCCESS if successful.
 **/
int openNextChapter(IndexZone *zone, Request *request)
  __attribute__((warn_unused_result));

/**
 * Determine the IndexRegion in which a block was found.
 *
 * @param zone               The zone that was searched
 * @param virtualChapter     The virtual chapter number
 *
 * @return the IndexRegion of the chapter in which the block was found
 **/
IndexRegion computeIndexRegion(const IndexZone *zone,
                               uint64_t         virtualChapter);

/**
 * Get a record from either the volume or the open chapter in a zone.
 *
 * @param zone           The index zone to query
 * @param request        The request originating the query
 * @param found          A pointer to a bool which will be set to
 *                       <code>true</code> if the record was found.
 * @param virtualChapter The chapter in which to search
 *
 * @return UDS_SUCCESS or an error code
 **/
int getRecordFromZone(IndexZone *zone,
                      Request   *request,
                      bool      *found,
                      uint64_t   virtualChapter)
  __attribute__((warn_unused_result));

/**
 * Put a record in the open chapter. If this fills the chapter, the chapter
 * will be closed and a new one will be opened.
 *
 * @param zone     The index zone containing the chapter
 * @param request  The request containing the name of the record
 * @param metadata The record metadata
 *
 * @return UDS_SUCCESS or an error
 **/
int putRecordInZone(IndexZone          *zone,
                    Request            *request,
                    const UdsChunkData *metadata)
  __attribute__((warn_unused_result));

/**
 * Search the cached sparse chapter index, either for a cached sparse hook, or
 * as the last chance for finding the record named by a request.
 *
 * @param [in]  zone            the index zone
 * @param [in]  request         the request originating the search
 * @param [in]  virtualChapter  if UINT64_MAX, search the entire cache;
 *                              otherwise search this chapter, if cached
 * @param [out] found           A pointer to a bool which will be set to
 *                              <code>true</code> if the record was found
 *
 * @return UDS_SUCCESS or an error code
 **/
int searchSparseCacheInZone(IndexZone *zone,
                            Request   *request,
                            uint64_t   virtualChapter,
                            bool      *found)
  __attribute__((warn_unused_result));

#endif /* INDEX_ZONE_H */
