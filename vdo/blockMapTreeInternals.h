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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapTreeInternals.h#16 $
 */

#ifndef BLOCK_MAP_TREE_INTERNALS_H
#define BLOCK_MAP_TREE_INTERNALS_H

#include "blockMapTree.h"

#include "blockMapFormat.h"
#include "blockMapPage.h"
#include "types.h"

/** A single page of a block map tree */
struct tree_page {
	/** struct waiter for a VIO to write out this page */
	struct waiter waiter;

	/** Dirty list entry */
	struct list_head entry;

	/**
	 * If this is a dirty tree page, the tree zone flush generation in which
	 * it was last dirtied.
	 */
	uint8_t generation;

	/** Whether this page is an interior tree page being written out. */
	bool writing;

	/**
	 * If this page is being written, the tree zone flush generation of the
	 * copy of the page being written.
	 **/
	uint8_t writing_generation;

	/**
	 * The earliest journal block containing uncommitted updates to this
	 * page
	 */
	sequence_number_t recovery_lock;

	/**
	 * The value of recovery_lock when the this page last started writing
	 */
	sequence_number_t writing_recovery_lock;

	/** The buffer to hold the on-disk representation of this page */
	char page_buffer[VDO_BLOCK_SIZE];
};

/**
 * An invalid PBN used to indicate that the page holding the location of a
 * tree root has been "loaded".
 **/
extern const physical_block_number_t INVALID_PBN;

/**
 * Extract the block_map_page from a tree_page.
 *
 * @param tree_page  The tree_page
 *
 * @return The block_map_page of the tree_page
 **/
static inline struct block_map_page * __must_check
as_vdo_block_map_page(struct tree_page *tree_page)
{
	return (struct block_map_page *) tree_page->page_buffer;
}

/**
 * Replace the VIOPool in a tree zone. This method is used by unit tests.
 *
 * @param zone       The zone whose pool is to be replaced
 * @param vdo        The vdo from which to make VIOs
 * @param pool_size  The size of the new pool
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
vdo_replace_tree_zone_vio_pool(struct block_map_tree_zone *zone,
			       struct vdo *vdo,
			       size_t pool_size);

/**
 * Check whether a buffer contains a valid page. If the page is bad, log an
 * error. If the page is valid, copy it to the supplied page.
 *
 * @param buffer  The buffer to validate (and copy)
 * @param nonce   The VDO nonce
 * @param pbn     The absolute PBN of the page
 * @param page    The page to copy into if valid
 *
 * @return <code>true</code> if the page was copied (valid)
 **/
bool vdo_copy_valid_page(char *buffer, nonce_t nonce,
			 physical_block_number_t pbn,
			 struct block_map_page *page);

#endif // BLOCK_MAP_TREE_INTERNALS_H
