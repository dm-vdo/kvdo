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
 * $Id: //eng/uds-releases/krusty/src/uds/volumeStore.h#2 $
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
 * @param volumeStore   The volume store
 **/
void closeVolumeStore(struct volume_store *volumeStore);

/**
 * Uninitialize a volume page buffer.
 *
 * @param volumePage  The volume page buffer
 **/
void destroyVolumePage(struct volume_page *volumePage);

/**
 * Get a pointer to the data contained in a volume page buffer.
 *
 * @param volumePage  The volume page buffer
 *
 * @return the address of the data
 **/
__attribute__((warn_unused_result))
static INLINE byte *getPageData(const struct volume_page *volumePage)
{
  return dm_bufio_get_block_data(volumePage->vp_buffer);
}

/**
 * Initialize a volume page buffer.
 *
 * @param geometry    The volume geometry
 * @param volumePage  The volume page buffer
 *
 * @return UDS_SUCCESS or an error status
 **/
int initializeVolumePage(const struct geometry *geometry,
                         struct volume_page    *volumePage)
  __attribute__((warn_unused_result));

/**
 * Open a volume store.
 *
 * @param volumeStore      The volume store
 * @param layout           The index layout
 * @param reservedBuffers  The number of buffers that can be reserved
 * @param bytesPerPage     The number of bytes in a volume page
 **/
int openVolumeStore(struct volume_store *volumeStore,
                    struct index_layout *layout,
                    unsigned int         reservedBuffers,
                    size_t               bytesPerPage)
  __attribute__((warn_unused_result));

/**
 * Prefetch volume pages into memory.
 *
 * @param volumeStore   The volume store
 * @param physicalPage  The volume page number of the first desired page
 * @param pageCount     The number of volume pages to prefetch
 **/
void prefetchVolumePages(const struct volume_store *volumeStore,
                         unsigned int               physicalPage,
                         unsigned int               pageCount);

/**
 * Prepare a buffer to write a page to the volume.
 *
 * @param volumeStore   The volume store
 * @param physicalPage  The volume page number of the desired page
 * @param volumePage    The volume page buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int prepareToWriteVolumePage(const struct volume_store *volumeStore,
                             unsigned int               physicalPage,
                             struct volume_page        *volumePage)
  __attribute__((warn_unused_result));

/**
 * Read a page from a volume store.
 *
 * @param volumeStore   The volume store
 * @param physicalPage  The volume page number of the desired page
 * @param volumePage    The volume page buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int readVolumePage(const struct volume_store *volumeStore,
                   unsigned int               physicalPage,
                   struct volume_page        *volumePage)
  __attribute__((warn_unused_result));

/**
 * Release a volume page buffer, because it will no longer be accessed before a
 * call to readVolumePage or prepareToWriteVolumePage.
 *
 * @param volumePage  The volume page buffer
 **/
void releaseVolumePage(struct volume_page *volumePage);

/**
 * Swap volume pages.  This is used to put the contents of a newly written
 * index page (in the scratch page) into the page cache.
 *
 * @param volumePage1  The volume page buffer
 * @param volumePage2  The volume page buffer
 **/
void swapVolumePages(struct volume_page *volumePage1,
                     struct volume_page *volumePage2);

/**
 * Sync the volume store to storage.
 *
 * @param volumeStore  The volume store
 *
 * @return UDS_SUCCESS or an error code
 **/
int syncVolumeStore(const struct volume_store *volumeStore)
  __attribute__((warn_unused_result));

/**
 * Write a page to a volume store.
 *
 * @param volumeStore   The volume store
 * @param physicalPage  The volume page number of the desired page
 * @param volumePage    The volume page buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int writeVolumePage(const struct volume_store *volumeStore,
                    unsigned int               physicalPage,
                    struct volume_page        *volumePage)
  __attribute__((warn_unused_result));

#endif /* VOLUME_STORE_H */
