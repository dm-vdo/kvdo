/*
 * Copyright Red Hat
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
 * $Id: //eng/uds-releases/lisa/src/uds/indexZone.h#1 $
 */

#ifndef INDEX_ZONE_H
#define INDEX_ZONE_H

#include "common.h"
#include "openChapterZone.h"
#include "request.h"

struct index_zone {
	struct uds_index *index;
	struct open_chapter_zone *open_chapter;
	struct open_chapter_zone *writing_chapter;
	uint64_t oldest_virtual_chapter;
	uint64_t newest_virtual_chapter;
	unsigned int id;
};

/**
 * Allocate an index zone.
 *
 * @param index        The index receiving the zone
 * @param zone_number  The number of the zone to allocate
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check make_index_zone(struct uds_index *index,
				 unsigned int zone_number);

/**
 * Clean up an index zone.
 *
 * @param zone The index zone to free
 **/
void free_index_zone(struct index_zone *zone);

/**
 * Check whether a chapter is sparse or dense based on the current state of
 * the index zone.
 *
 * @param zone             The index zone to check against
 * @param virtual_chapter  The virtual chapter number of the chapter to check
 *
 * @return true if the chapter is in the sparse part of the volume
 **/
bool __must_check is_zone_chapter_sparse(const struct index_zone *zone,
					 uint64_t virtual_chapter);

/**
 * Set the active chapter numbers for a zone based on its index. The active
 * chapters consist of the range of chapters from the current oldest to
 * the current newest virtual chapter.
 *
 * @param zone          The zone to set
 **/
void set_active_chapters(struct index_zone *zone);

/**
 * Dispatch a control request to an index zone.
 *
 * @param request The request to dispatch
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
dispatch_index_zone_control_request(struct uds_request *request);

/**
 * Open the next chapter.
 *
 * @param zone    The zone containing the open chapter
 * @param request The request which requires the next chapter to be
 *                opened
 *
 * @return UDS_SUCCESS if successful.
 **/
int __must_check open_next_chapter(struct index_zone *zone,
				   struct uds_request *request);

/**
 * Determine the index region in which a block was found.
 *
 * @param zone                The zone that was searched
 * @param virtual_chapter     The virtual chapter number
 *
 * @return the index region of the chapter in which the block was found
 **/
enum uds_index_region compute_index_region(const struct index_zone *zone,
					   uint64_t virtual_chapter);

/**
 * Get a record from either the volume or the open chapter in a zone.
 *
 * @param zone             The index zone to query
 * @param request          The request originating the query
 * @param found            A pointer to a bool which will be set to
 *                         <code>true</code> if the record was found.
 * @param virtual_chapter  The chapter in which to search
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_record_from_zone(struct index_zone *zone,
				      struct uds_request *request,
				      bool *found,
				      uint64_t virtual_chapter);

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
int __must_check put_record_in_zone(struct index_zone *zone,
				    struct uds_request *request,
				    const struct uds_chunk_data *metadata);

/**
 * Search the cached sparse chapter index, either for a cached sparse hook, or
 * as the last chance for finding the record named by a request.
 *
 * @param [in]  zone             the index zone
 * @param [in]  request          the request originating the search
 * @param [in]  virtual_chapter  if UINT64_MAX, search the entire cache;
 *                               otherwise search this chapter, if cached
 * @param [out] found            A pointer to a bool which will be set to
 *                               <code>true</code> if the record was found
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check search_sparse_cache_in_zone(struct index_zone *zone,
					     struct uds_request *request,
					     uint64_t virtual_chapter,
					     bool *found);

#endif /* INDEX_ZONE_H */
