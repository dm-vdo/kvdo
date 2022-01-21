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
 * $Id: //eng/uds-releases/krusty/src/uds/volumeStore.c#21 $
 */

#include "geometry.h"
#include "indexLayout.h"
#include "logger.h"
#include "volumeStore.h"


/**********************************************************************/
void close_volume_store(struct volume_store *volume_store)
{
	if (volume_store->vs_client != NULL) {
		dm_bufio_client_destroy(volume_store->vs_client);
		volume_store->vs_client = NULL;
	}
}

/**********************************************************************/
void destroy_volume_page(struct volume_page *volume_page)
{
	release_volume_page(volume_page);
}

/**********************************************************************/
int initialize_volume_page(const struct geometry *geometry,
			   struct volume_page *volume_page)
{
	volume_page->vp_buffer = NULL;
	return UDS_SUCCESS;
}

/**********************************************************************/
int open_volume_store(struct volume_store *volume_store,
		      struct index_layout *layout,
		      unsigned int reserved_buffers __maybe_unused,
		      size_t bytes_per_page)
{
	return open_uds_volume_bufio(layout, bytes_per_page, reserved_buffers,
				     &volume_store->vs_client);
}

/**********************************************************************/
void prefetch_volume_pages(const struct volume_store *vs __maybe_unused,
			   unsigned int physical_page __maybe_unused,
			   unsigned int page_count __maybe_unused)
{
	dm_bufio_prefetch(vs->vs_client, physical_page, page_count);
}

/**********************************************************************/
int prepare_to_write_volume_page(const struct volume_store *volume_store
				 __maybe_unused,
				 unsigned int physical_page __maybe_unused,
				 struct volume_page *volume_page
				 __maybe_unused)
{
	struct dm_buffer *buffer = NULL;
	byte *data;
	release_volume_page(volume_page);
	data = dm_bufio_new(volume_store->vs_client, physical_page, &buffer);
	if (IS_ERR(data)) {
		return -PTR_ERR(data);
	}
	volume_page->vp_buffer = buffer;
	return UDS_SUCCESS;
}

/**********************************************************************/
int read_volume_page(const struct volume_store *volume_store,
		     unsigned int physical_page,
		     struct volume_page *volume_page)
{
	byte *data;
	release_volume_page(volume_page);
	data = dm_bufio_read(volume_store->vs_client, physical_page,
			     &volume_page->vp_buffer);
	if (IS_ERR(data)) {
		return uds_log_warning_strerror(-PTR_ERR(data),
						"error reading physical page %u",
						physical_page);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
void release_volume_page(struct volume_page *volume_page __maybe_unused)
{
	if (volume_page->vp_buffer != NULL) {
		dm_bufio_release(volume_page->vp_buffer);
		volume_page->vp_buffer = NULL;
	}
}

/**********************************************************************/
void swap_volume_pages(struct volume_page *volume_page1,
		       struct volume_page *volume_page2)
{
	struct volume_page temp = *volume_page1;
	*volume_page1 = *volume_page2;
	*volume_page2 = temp;
}

/**********************************************************************/
int sync_volume_store(const struct volume_store *volume_store)
{
	int result = -dm_bufio_write_dirty_buffers(volume_store->vs_client);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot sync chapter to volume");
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int write_volume_page(const struct volume_store *volume_store,
		      unsigned int physical_page,
		      struct volume_page *volume_page)
{
	dm_bufio_mark_buffer_dirty(volume_page->vp_buffer);
	return UDS_SUCCESS;
}
