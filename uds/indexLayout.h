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
 * $Id: //eng/uds-releases/krusty/src/uds/indexLayout.h#8 $
 */

#ifndef INDEX_LAYOUT_H
#define INDEX_LAYOUT_H

#include "buffer.h"
#include "indexState.h"
#include "indexVersion.h"
#include "ioFactory.h"
#include "uds.h"

struct index_layout;

/**
 * Construct an index layout.  This is a platform specific function that uses
 * the name string, a flag that indicates old vs. new indices, and a
 * UDS configuration (for new indices) to make an IO factory and invoke
 * make_index_layout_from_factory.
 *
 * @param name        String naming the index.  Each platform will use its own
 *                    conventions to interpret the string, but in general it is
 *                    a space-separated sequence of param=value settings.  For
 *                    backward compatibility a string without an equals is
 *                    treated as a platform-specific default parameter value.
 * @param new_layout  Whether this is a new layout.
 * @param config      The UDS configuration required for a new layout.
 * @param layout_ptr  Where to store the new index layout
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check make_index_layout(const char *name,
				   bool new_layout,
				   const struct uds_configuration *config,
				   struct index_layout **layout_ptr);

/**
 * Construct an index layout using an IO factory.  This method is
 * common to all platforms.
 *
 * @param factory     The IO factory for the block storage containing the index.
 * @param offset      The offset of the start of the index within the block
 *                    storage address space.
 * @param named_size  The size in bytes of the space within the block storage
 *                    address space, as specified in the name string.
 * @param new_layout  Whether this is a new layout.
 * @param config      The UDS configuration required for a new layout.
 * @param layout_ptr  Where to store the new index layout
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
make_index_layout_from_factory(struct io_factory *factory,
			       off_t offset,
			       uint64_t named_size,
			       bool new_layout,
			       const struct uds_configuration *config,
			       struct index_layout **layout_ptr);

/**
 * Decrement the use count of an index layout.  If the count goes to zero, free
 * the index layout.
 *
 * @param layout_ptr  Where the layout is being stored.  Always reset to NULL.
 **/
void put_index_layout(struct index_layout **layout_ptr);

/*****************************************************************************/
int __must_check
cancel_index_save(struct index_layout *layout, unsigned int save_slot);

/*****************************************************************************/
int __must_check
commit_index_save(struct index_layout *layout, unsigned int save_slot);

/*****************************************************************************/
int __must_check discard_index_saves(struct index_layout *layout, bool all);

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
int __must_check
find_latest_index_save_slot(struct index_layout *layout,
			    unsigned int *num_zones_ptr,
			    unsigned int *slot_ptr);

/**
 * Get another reference to an index layout, incrementing it's use count.
 *
 * @param layout      The index layout.
 * @param layout_ptr  Where the new layout pointer is being stored.
 **/
void get_index_layout(struct index_layout *layout,
		      struct index_layout **layout_ptr);

/**
 * Open a BufferedReader for a specified state, kind, and zone.
 *
 * @param layout      The index layout
 * @param slot        The save slot
 * @param kind        The kind if index save region to open.
 * @param zone        The zone number for the region.
 * @param reader_ptr  Where to store the BufferedReader.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
open_index_buffered_reader(struct index_layout *layout,
			   unsigned int slot,
			   RegionKind kind,
			   unsigned int zone,
			   BufferedReader **reader_ptr);

/**
 * Open a BufferedWriter for a specified state, kind, and zone.
 *
 * @param layout      The index layout
 * @param slot        The save slot
 * @param kind        The kind if index save region to open.
 * @param zone        The zone number for the region.
 * @param writer_ptr  Where to store the BufferedWriter.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
open_index_buffered_writer(struct index_layout *layout,
			   unsigned int slot,
			   RegionKind kind,
			   unsigned int zone,
			   BufferedWriter **writer_ptr);

/**
 * Obtain the nonce to be used to store or validate the loading of volume index
 * pages.
 *
 * @param [in]  layout   The index layout.
 *
 * @return The nonce to use.
 **/
uint64_t __must_check get_volume_nonce(struct index_layout *layout);

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
int __must_check open_volume_bufio(struct index_layout *layout,
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
int __must_check
verify_index_config(struct index_layout *layout,
                    struct uds_configuration *config);

/**
 * Determine which index save slot to use for a new index save.
 *
 * Also allocates the masterIndex regions and, if needed, the openChapter
 * region.
 *
 * @param [in]  layout          The index layout.
 * @param [in]  num_zones       Actual number of zones currently in use.
 * @param [in]  save_type       The index save type.
 * @param [out] save_slot_ptr   Where to store the save slot number.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check setup_index_save_slot(struct index_layout *layout,
				       unsigned int num_zones,
				       IndexSaveType save_type,
				       unsigned int *save_slot_ptr);

/**
 * Write the index configuration.
 *
 * @param layout  the generic index layout
 * @param config  the index configuration to write
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
write_index_config(struct index_layout *layout,
                   struct uds_configuration *config);

/**
 * Get the index state buffer
 *
 * @param layout  the index layout
 * @param slot    the save slot
 *
 * @return UDS_SUCCESS or an error code
 **/
struct buffer * __must_check
get_index_state_buffer(struct index_layout *layout, unsigned int slot);

/**
 * Get the index version parameters.
 *
 * @param layout  the index layout
 *
 * @return the index version parameters.
 **/
const struct index_version * __must_check
get_index_version(struct index_layout *layout);

#endif // INDEX_LAYOUT_H
