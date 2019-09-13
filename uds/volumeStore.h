/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/src/uds/volumeStore.h#1 $
 */

#ifndef VOLUME_PAGE_H
#define VOLUME_PAGE_H

#include <linux/dm-bufio.h>

struct indexLayout;

/*
 * A volume store is the actual storage behind the volume.  More precisely it
 * contains a pointer to the platform specific mechanism for accessing that
 * storage.  In kernel mode, we use a block device that is accessed by the
 * dm-bufio module.  In user mode, we use a file system file that is accessed
 * by our ioRegion module.  All of the platform specific code is framed by
 * using #ifdef __KERNEL__, and is contained in the volumeStore module.
 */

struct volume_store {
  struct dm_bufio_client *vs_client;
};

/*
 * A volume page is the actual buffer for a single record or index page.  In
 * kernel mode, the actual memory is allocated by and managed by the dm-bufio
 * module.  In user mode, this module allocates and frees the actual memory.
 */

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
 * Open a volume store.
 *
 * @param volumeStore      The volume store
 * @param layout           The index layout
 * @param reservedBuffers  The number of buffers that can be reserved
 * @param bytesPerPage     The number of bytes in a volume page
 **/
int openVolumeStore(struct volume_store *volumeStore,
                    struct indexLayout  *layout,
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
 * Sync the volume store to storage.
 *
 * @param volumeStore  The volume store
 *
 * @return UDS_SUCCESS or an error code
 **/
int syncVolumeStore(const struct volume_store *volumeStore)
  __attribute__((warn_unused_result));

#endif /* VOLUME_PAGE_H */
