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
 * $Id: //eng/uds-releases/krusty/src/uds/indexPageMap.h#6 $
 */

#ifndef INDEX_PAGE_MAP_H
#define INDEX_PAGE_MAP_H 1

#include "common.h"
#include "geometry.h"
#include "indexComponent.h"

extern const struct index_component_info INDEX_PAGE_MAP_INFO;

struct index_page_bounds {
  unsigned int lowestList;
  unsigned int highestList;
};

/*
 *  Notes on struct index_page_map
 *
 *  Each volume maintains an index page map which records how the chapter delta
 *  lists are distributed among the index pages for that chapter.
 *
 *  The map is conceptually a two-dimensional array indexed by chapter number
 *  and index page number within the chapter.  Each entry contains the number
 *  of the last delta list on that index page.  In order to save memory, the
 *  information for the last page in each chapter is not recorded, as it is
 *  known from the geometry.
 */

typedef uint16_t index_page_map_entry_t;

struct index_page_map {
  const struct geometry  *geometry;
  uint64_t                lastUpdate;
  index_page_map_entry_t      *entries;
};

/**
 * Create an index page map.
 *
 * @param geometry     The geometry governing the index.
 * @param mapPtr       A pointer to hold the new map.
 *
 * @return             A success or error code.
 **/
int __must_check
makeIndexPageMap(const struct geometry *geometry, struct index_page_map **mapPtr);

/**
 * Free an index page map.
 *
 * @param map  The index page map to destroy.
 **/
void freeIndexPageMap(struct index_page_map *map);

/**
 * Get the virtual chapter number of the last update to the index page map.
 *
 * @param map   The index page map
 *
 * @return the virtual chapter number of the last chapter updated
 **/
uint64_t getLastUpdate(const struct index_page_map *map);

/**
 * Update an index page map entry.
 *
 * @param map                   The map to update
 * @param virtualChapterNumber  The virtual chapter number being updated.
 * @param chapterNumber         The chapter of the entry to update
 * @param indexPageNumber       The index page of the entry to update
 * @param deltaListNumber       The value of the new entry
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check updateIndexPageMap(struct index_page_map *map,
				    uint64_t virtualChapterNumber,
				    unsigned int chapterNumber,
				    unsigned int indexPageNumber,
				    unsigned int deltaListNumber);

/**
 * Find the page number of the index page in a chapter that will contain the
 * chapter index entry for a given chunk name, if it exists.
 *
 * @param [in]  map                 The map to search
 * @param [in]  name                The chunk name
 * @param [in]  chapterNumber       The chapter containing the index page
 * @param [out] indexPageNumberPtr  A pointer to hold the result, guaranteed to
 *                                  be a valid index page number on UDS_SUCCESS
 *
 * @return UDS_SUCCESS, or UDS_INVALID_ARGUMENT if the chapter number
 *         is out of range
 **/
int __must_check findIndexPageNumber(const struct index_page_map *map,
				     const struct uds_chunk_name *name,
				     unsigned int chapterNumber,
				     unsigned int *indexPageNumberPtr);

/**
 * Get the lowest and highest numbered delta lists for the given immutable
 * chapter index page from the index page map.
 *
 * @param map             The index page map
 * @param chapterNumber   The chapter containing the delta list
 * @param indexPageNumber The index page number within the chapter
 * @param bounds          A structure to hold the list number bounds
 *                        for the given page
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check getListNumberBounds(const struct index_page_map *map,
				     unsigned int chapterNumber,
				     unsigned int indexPageNumber,
				     struct index_page_bounds *bounds);

/**
 * Compute the size of the index page map save image, including all headers.
 *
 * @param geometry      The index geometry.
 *
 * @return The number of bytes required to save the index page map.
 **/
uint64_t computeIndexPageMapSaveSize(const struct geometry *geometry);

/**
 * Escaped for testing....
 *
 * @param geometry      The index geometry.
 *
 * @return              The number of bytes required for the page map data,
 *                      exclusive of headers.
 **/
size_t __must_check indexPageMapSize(const struct geometry *geometry);

#endif // INDEX_PAGE_MAP_H
