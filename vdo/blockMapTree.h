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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapTree.h#23 $
 */

#ifndef BLOCK_MAP_TREE_H
#define BLOCK_MAP_TREE_H

#include <linux/list.h>

#include "blockMapFormat.h"
#include "blockMapPage.h"
#include "constants.h"
#include "kernelTypes.h"
#include "types.h"
#include "waitQueue.h"

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
extern const physical_block_number_t VDO_INVALID_PBN;

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

/**
 * Intialize a block_map_tree_zone.
 *
 * @param zone         The block_map_zone of the tree zone to intialize
 * @param vdo          The vdo
 * @param maximum_age  The number of journal blocks before a dirtied page is
 *                     considered old and may be written out
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check vdo_initialize_tree_zone(struct block_map_zone *zone,
					  struct vdo *vdo,
					  block_count_t maximum_age);

/**
 * Clean up a block_map_tree_zone.
 *
 * @param tree_zone  The zone to clean up
 **/
void vdo_uninitialize_block_map_tree_zone(struct block_map_tree_zone *tree_zone);

/**
 * Set the initial dirty period for a tree zone.
 *
 * @param tree_zone  The tree zone
 * @param period     The initial dirty period to set
 **/
void vdo_set_tree_zone_initial_period(struct block_map_tree_zone *tree_zone,
				      sequence_number_t period);

/**
 * Check whether a tree zone is active (i.e. has any active lookups,
 * outstanding I/O, or pending I/O).
 *
 * @param zone  The zone to check
 *
 * @return <code>true</code> if the zone is active
 **/
bool __must_check vdo_is_tree_zone_active(struct block_map_tree_zone *zone);

/**
 * Advance the dirty period for a tree zone.
 *
 * @param zone    The block_map_tree_zone to advance
 * @param period  The new dirty period
 **/
void vdo_advance_zone_tree_period(struct block_map_tree_zone *zone,
				  sequence_number_t period);

/**
 * Drain the zone trees, i.e. ensure that all I/O is quiesced. If required by
 * the drain type, all dirty block map trees will be written to disk. This
 * method must not be called when lookups are active.
 *
 * @param zone  The block_map_tree_zone to drain
 **/
void vdo_drain_zone_trees(struct block_map_tree_zone *zone);

/**
 * Look up the PBN of the block map page for a data_vio's LBN in the arboreal
 * block map. If necessary, the block map page will be allocated. Also, the
 * ancestors of the block map page will be allocated or loaded if necessary.
 *
 * @param data_vio  The data_vio requesting the lookup
 **/
void vdo_lookup_block_map_pbn(struct data_vio *data_vio);

/**
 * Find the PBN of a leaf block map page. This method may only be used after
 * all allocated tree pages have been loaded, otherwise, it may give the wrong
 * answer (0).
 *
 * @param map          The block map containing the forest
 * @param page_number  The page number of the desired block map page
 *
 * @return The PBN of the page
 **/
physical_block_number_t vdo_find_block_map_page_pbn(struct block_map *map,
						    page_number_t page_number);

/**
 * Write a tree page or indicate that it has been re-dirtied if it is already
 * being written. This method is used when correcting errors in the tree during
 * read-only rebuild.
 *
 * @param page  The page to write
 * @param zone  The tree zone managing the page
 **/
void vdo_write_tree_page(struct tree_page *page, struct block_map_tree_zone *zone);

#endif // BLOCK_MAP_TREE_H
