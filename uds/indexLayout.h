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
 * $Id: //eng/uds-releases/lisa/src/uds/indexLayout.h#7 $
 */

#ifndef INDEX_LAYOUT_H
#define INDEX_LAYOUT_H

#include "buffer.h"
#include "config.h"
#include "indexState.h"
#include "ioFactory.h"
#include "uds.h"

struct index_layout;

/**
 * Construct an index layout.
 *
 * @param name        String naming the index.  Each platform will use its own
 *                    conventions to interpret the string, but in general it is
 *                    a space-separated sequence of param=value settings.  For
 *                    backward compatibility a string without an equals is
 *                    treated as a platform-specific default parameter value.
 * @param new_layout  Whether this is a new layout.
 * @param config      The configuration required for a new layout.
 * @param layout_ptr  Where to store the new index layout
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check make_uds_index_layout(const char *name,
				       bool new_layout,
				       const struct configuration *config,
				       struct index_layout **layout_ptr);

/**
 * Get another reference to an index layout, incrementing its use count.
 *
 * @param layout      The index layout.
 * @param layout_ptr  Where the new layout pointer is being stored.
 **/
void get_uds_index_layout(struct index_layout *layout,
			  struct index_layout **layout_ptr);

/**
 * Decrement the use count of an index layout.  If the count goes to zero, free
 * the index layout.
 *
 * @param layout  The layout to release or free
 **/
void put_uds_index_layout(struct index_layout *layout);

/**********************************************************************/
int __must_check cancel_uds_index_save(struct index_layout *layout,
				       unsigned int save_slot);

/**********************************************************************/
int __must_check commit_uds_index_save(struct index_layout *layout,
				       unsigned int save_slot);

/**********************************************************************/
int __must_check discard_uds_index_saves(struct index_layout *layout);

/**
 * Find the latest index save slot.
 *
 * @param [in]  layout          The single file layout.
 * @param [out] num_zones_ptr   Where to store the actual number of zones
 *                                that were saved.
 * @param [out] slot_ptr        Where to store the slot number we found.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check find_latest_uds_index_save_slot(struct index_layout *layout,
						 unsigned int *num_zones_ptr,
						 unsigned int *slot_ptr);

/**
 * Open a buffered reader for a specified state, kind, and zone.
 *
 * @param layout      The index layout
 * @param slot        The save slot
 * @param kind        The kind of index save region to open.
 * @param zone        The zone number for the region.
 * @param reader_ptr  Where to store the buffered reader.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
open_uds_index_buffered_reader(struct index_layout *layout,
			       unsigned int slot,
			       enum region_kind kind,
			       unsigned int zone,
			       struct buffered_reader **reader_ptr);

/**
 * Open a buffered writer for a specified state, kind, and zone.
 *
 * @param layout      The index layout
 * @param slot        The save slot
 * @param kind        The kind of index save region to open.
 * @param zone        The zone number for the region.
 * @param writer_ptr  Where to store the buffered writer.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
open_uds_index_buffered_writer(struct index_layout *layout,
			       unsigned int slot,
			       enum region_kind kind,
			       unsigned int zone,
			       struct buffered_writer **writer_ptr);

/**
 * Obtain the nonce to be used to store or validate the loading of volume index
 * pages.
 *
 * @param [in]  layout   The index layout.
 *
 * @return The nonce to use.
 **/
uint64_t __must_check get_uds_volume_nonce(struct index_layout *layout);

/**
 * Obtain a dm_bufio_client for the specified index volume.
 *
 * @param [in]  layout            The index layout.
 * @param [in]  block_size        The size of a volume page
 * @param [in]  reserved_buffers  The count of reserved buffers
 * @param [out] client_ptr        Where to put the new dm_bufio_client
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check open_uds_volume_bufio(struct index_layout *layout,
				       size_t block_size,
				       unsigned int reserved_buffers,
				       struct dm_bufio_client **client_ptr);

/**
 * Read the index configuration, and verify that it matches the given
 * configuration.
 *
 * @param layout  the generic index layout
 * @param config  the index configuration
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check verify_uds_index_config(struct index_layout *layout,
					 struct configuration *config);

/**
 * Determine which index save slot to use for a new index save.
 *
 * Also allocates the volume index regions and the openChapter region.
 *
 * @param [in]  layout          The index layout.
 * @param [in]  num_zones       Actual number of zones currently in use.
 * @param [out] save_slot_ptr   Where to store the save slot number.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check setup_uds_index_save_slot(struct index_layout *layout,
					   unsigned int num_zones,
					   unsigned int *save_slot_ptr);

/**
 * Write the index configuration.
 *
 * @param layout  the generic index layout
 * @param config  the index configuration to write
 * @param offset  A block offset to apply when writing the configuration
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check write_uds_index_config(struct index_layout *layout,
					struct configuration *config,
					off_t offset);

/**
 * Get the index state buffer
 *
 * @param layout  the index layout
 * @param slot    the save slot
 *
 * @return UDS_SUCCESS or an error code
 **/
struct buffer *__must_check
get_uds_index_state_buffer(struct index_layout *layout, unsigned int slot);

#endif // INDEX_LAYOUT_H
