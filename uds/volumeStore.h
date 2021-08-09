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
 * $Id: //eng/uds-releases/lisa/src/uds/volumeStore.h#1 $
 */

#ifndef VOLUME_STORE_H
#define VOLUME_STORE_H

#include "common.h"
#include "compiler.h"
#include "memoryAlloc.h"

#include <linux/dm-bufio.h>

struct geometry;
struct index_layout;


struct volume_store {
	struct dm_bufio_client *vs_client;
};


struct volume_page {
	struct dm_buffer *vp_buffer;
};

/**
 * Close a volume store.
 *
 * @param volume_store   The volume store
 **/
void close_volume_store(struct volume_store *volume_store);

/**
 * Uninitialize a volume page buffer.
 *
 * @param volume_page  The volume page buffer
 **/
void destroy_volume_page(struct volume_page *volume_page);

/**
 * Get a pointer to the data contained in a volume page buffer.
 *
 * @param volume_page  The volume page buffer
 *
 * @return the address of the data
 **/
static INLINE byte *__must_check
get_page_data(const struct volume_page *volume_page)
{
	return dm_bufio_get_block_data(volume_page->vp_buffer);
}

/**
 * Initialize a volume page buffer.
 *
 * @param geometry    The volume geometry
 * @param volume_page  The volume page buffer
 *
 * @return UDS_SUCCESS or an error status
 **/
int __must_check initialize_volume_page(const struct geometry *geometry,
					struct volume_page *volume_page);

/**
 * Open a volume store.
 *
 * @param volume_store      The volume store
 * @param layout            The index layout
 * @param reserved_buffers  The number of buffers that can be reserved
 * @param bytes_per_page    The number of bytes in a volume page
 **/
int __must_check open_volume_store(struct volume_store *volume_store,
				   struct index_layout *layout,
				   unsigned int reserved_buffers,
				   size_t bytes_per_page);

/**
 * Prefetch volume pages into memory.
 *
 * @param volume_store   The volume store
 * @param physical_page  The volume page number of the first desired page
 * @param page_count     The number of volume pages to prefetch
 **/
void prefetch_volume_pages(const struct volume_store *volume_store,
			   unsigned int physical_page,
			   unsigned int page_count);

/**
 * Prepare a buffer to write a page to the volume.
 *
 * @param volume_store   The volume store
 * @param physical_page  The volume page number of the desired page
 * @param volume_page    The volume page buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
prepare_to_write_volume_page(const struct volume_store *volume_store,
			     unsigned int physical_page,
			     struct volume_page *volume_page);

/**
 * Read a page from a volume store.
 *
 * @param volume_store   The volume store
 * @param physical_page  The volume page number of the desired page
 * @param volume_page    The volume page buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check read_volume_page(const struct volume_store *volume_store,
				  unsigned int physical_page,
				  struct volume_page *volume_page);

/**
 * Release a volume page buffer, because it will no longer be accessed before a
 * call to read_volume_page or prepare_to_write_volume_page.
 *
 * @param volume_page  The volume page buffer
 **/
void release_volume_page(struct volume_page *volume_page);

/**
 * Swap volume pages.  This is used to put the contents of a newly written
 * index page (in the scratch page) into the page cache.
 *
 * @param volume_page1  The volume page buffer
 * @param volume_page2  The volume page buffer
 **/
void swap_volume_pages(struct volume_page *volume_page1,
		       struct volume_page *volume_page2);

/**
 * Sync the volume store to storage.
 *
 * @param volume_store  The volume store
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check sync_volume_store(const struct volume_store *volume_store);

/**
 * Write a page to a volume store.
 *
 * @param volume_store   The volume store
 * @param physical_page  The volume page number of the desired page
 * @param volume_page    The volume page buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check write_volume_page(const struct volume_store *volume_store,
				   unsigned int physical_page,
				   struct volume_page *volume_page);

#endif /* VOLUME_STORE_H */
