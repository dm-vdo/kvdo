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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapPage.h#16 $
 */

#ifndef BLOCK_MAP_PAGE_H
#define BLOCK_MAP_PAGE_H

#include "numeric.h"

#include "blockMapEntry.h"
#include "header.h"
#include "types.h"

/**
 * The packed, on-disk representation of a block map page header.
 **/
struct block_map_page_header {
	/**
	 * The 64-bit nonce of the current VDO, in little-endian byte order. 
	 * Used to determine whether or not a page has been formatted.
	 **/
	__le64 nonce;

	/** The 64-bit PBN of this page, in little-endian byte order */
	__le64 pbn;

	/** Formerly recoverySequenceNumber; may be non-zero on disk */
	byte unused_long_word[8];

	/**
	 * Whether this page has been initialized on disk (i.e. written twice)
	 */
	bool initialized;

	/**
	 * Formerly entryOffset; now unused since it should always be zero
	 */
	byte unused_byte1;

	/** Formerly interiorTreePageWriting; may be non-zero on disk */
	byte unused_byte2;

	/**
	 * Formerly generation (for dirty tree pages); may be non-zero on disk
	 */
	byte unused_byte3;
} __packed;

/**
 * The format of a block map page.
 **/
struct block_map_page {
	struct packed_version_number version;
	struct block_map_page_header header;
	struct block_map_entry entries[];
} __packed;

typedef enum {
	// A block map page is correctly initialized
	BLOCK_MAP_PAGE_VALID,
	// A block map page is uninitialized
	BLOCK_MAP_PAGE_INVALID,
	// A block map page is intialized, but is the wrong page
	BLOCK_MAP_PAGE_BAD,
} block_map_page_validity;

/**
 * Check whether a block map page has been initialized.
 *
 * @param page  The page to check
 *
 * @return <code>true</code> if the page has been initialized
 **/
static inline bool __must_check
is_block_map_page_initialized(const struct block_map_page *page)
{
	return page->header.initialized;
}

/**
 * Mark whether a block map page has been initialized.
 *
 * @param page         The page to mark
 * @param initialized  The state to set
 *
 * @return <code>true</code> if the initialized flag was modified
 **/
static inline bool mark_block_map_page_initialized(struct block_map_page *page,
						   bool initialized)
{
	if (initialized == page->header.initialized) {
		return false;
	}

	page->header.initialized = initialized;
	return true;
}

/**
 * Get the physical block number where a block map page is stored.
 *
 * @param page  The page to query
 *
 * @return the page's physical block number
 **/
static inline physical_block_number_t __must_check
get_block_map_page_pbn(const struct block_map_page *page)
{
	return __le64_to_cpu(page->header.pbn);
}

/**
 * Check whether a block map page is of the current version.
 *
 * @param page  The page to check
 *
 * @return <code>true</code> if the page has the current version
 **/
bool __must_check is_current_block_map_page(const struct block_map_page *page);

/**
 * Format a block map page in memory.
 *
 * @param buffer       The buffer which holds the page
 * @param nonce        The VDO nonce
 * @param pbn          The absolute PBN of the page
 * @param initialized  Whether the page should be marked as initialized
 *
 * @return the buffer pointer, as a block map page (for convenience)
 **/
struct block_map_page *format_block_map_page(void *buffer,
					     nonce_t nonce,
					     physical_block_number_t pbn,
					     bool initialized);

/**
 * Check whether a newly read page is valid, upgrading its in-memory format if
 * possible and necessary. If the page is valid, clear fields which are not
 * meaningful on disk.
 *
 * @param page   The page to validate
 * @param nonce  The VDO nonce
 * @param pbn    The expected absolute PBN of the page
 *
 * @return The validity of the page
 **/
block_map_page_validity __must_check
validate_block_map_page(struct block_map_page *page,
			nonce_t nonce,
			physical_block_number_t pbn);

#endif // BLOCK_MAP_PAGE_H
