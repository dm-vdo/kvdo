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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapPage.h#5 $
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
typedef union __attribute__((packed)) {
	struct __attribute__((packed)) {
		/**
		 * The 64-bit nonce of the current VDO, in little-endian byte
		 * order. Used to determine whether or not a page has been
		 * formatted.
		 **/
		byte nonce[8];

		/** The 64-bit PBN of this page, in little-endian byte order */
		byte pbn[8];

		/** Formerly recoverySequenceNumber; may be non-zero on disk */
		byte unused_long_word[8];

		/**
		 * Whether this page has been initialized on disk (i.e. written
		 * twice)
		 */
		bool initialized;

		/**
		 * Formerly entryOffset; now unused since it should always be
		 * zero
		 */
		byte unused_byte1;

		/** Formerly interiorTreePageWriting; may be non-zero on disk */
		byte unused_byte2;

		/**
		 * Formerly generation (for dirty tree pages); may be non-zero
		 * on disk
		 */
		byte unused_byte3;
	} fields;

	// A raw view of the packed encoding.
	uint8_t raw[8 + 8 + 8 + 1 + 1 + 1 + 1];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	// This view is only valid on little-endian machines and is only present
	// for ease of directly examining packed entries in GDB.
	struct __attribute__((packed)) {
		uint64_t nonce;
		PhysicalBlockNumber pbn;
		uint64_t unused_long_word;
		bool initialized;
		uint8_t unused_byte1;
		uint8_t unused_byte2;
		uint8_t unused_byte3;
	} little_endian;
#endif
} PageHeader;

/**
 * The format of a block map page.
 **/
struct block_map_page {
	struct packed_version_number version;
	PageHeader header;
	BlockMapEntry entries[];
} __attribute__((packed));

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
__attribute__((warn_unused_result)) static inline bool
is_block_map_page_initialized(const struct block_map_page *page)
{
	return page->header.fields.initialized;
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
	if (initialized == page->header.fields.initialized) {
		return false;
	}

	page->header.fields.initialized = initialized;
	return true;
}

/**
 * Get the physical block number where a block map page is stored.
 *
 * @param page  The page to query
 *
 * @return the page's physical block number
 **/
__attribute__((warn_unused_result)) static inline PhysicalBlockNumber
get_block_map_page_pbn(const struct block_map_page *page)
{
	return getUInt64LE(page->header.fields.pbn);
}

/**
 * Check whether a block map page is of the current version.
 *
 * @param page  The page to check
 *
 * @return <code>true</code> if the page has the current version
 **/
bool is_current_block_map_page(const struct block_map_page *page)
	__attribute__((warn_unused_result));

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
					     Nonce nonce,
					     PhysicalBlockNumber pbn,
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
block_map_page_validity validate_block_map_page(struct block_map_page *page,
						Nonce nonce,
						PhysicalBlockNumber pbn)
	__attribute__((warn_unused_result));

/**
 * Update an entry on a block map page.
 *
 * @param [in]     page           The page to update
 * @param [in]     data_vio       The data_vio making the update
 * @param [in]     pbn            The new PBN for the entry
 * @param [in]     mapping_state  The new mapping state for the entry
 * @param [in,out] recovery_lock  A reference to the current recovery sequence
 *                                number lock held by the page. Will be updated
 *                                if the lock changes to protect the new entry
 **/
void update_block_map_page(struct block_map_page *page,
			   struct data_vio *data_vio,
			   PhysicalBlockNumber pbn,
			   BlockMappingState mapping_state,
			   SequenceNumber *recovery_lock);

#endif // BLOCK_MAP_PAGE_H
