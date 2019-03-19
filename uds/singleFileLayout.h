/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/gloria/src/uds/singleFileLayout.h#1 $
 */

#ifndef SINGLE_FILE_LAYOUT_H
#define SINGLE_FILE_LAYOUT_H

#include "indexLayout.h"

/**
 * A SingleFileLayout object is used to manage the persistent storage of
 * an Albireo local index using a single file or block device. It is
 * capable of constructing IndexState instances for each sub-index of the
 * Albireo index, as well as managing the persistence of the index
 * configuration.
 **/
typedef struct singleFileLayout SingleFileLayout;

/**
 * Create a new single file layout for a new index.
 *
 * @param region     The access to the underlying stable storage system;
 *                     the new layout will own the region and close it.
 * @param offset     The offset of the start of the index within the device or
 *                     file's address space.
 * @param size       The size in bytes of the space within the file or device's
 *                     address space, must be at least as large as that
 *                     returned by udsComputeIndexSize() or an error will
 *                     result.
 * @param config     A properly-initialized index configuration.
 * @param layoutPtr  Where to store the new layout object.
 *
 * @return UDS_SUCCESS or an error code, specifically
 *         UDS_CONF_REQUIRED if the configuration is not provided,
 *         UDS_INDEX_NAME_REQUIRED if the name parameter is required by the
 *              platform and not specified or invalid,
 *         UDS_INDEX_EXISTS on platforms where index is a file if the file
 *              already exists,
 *         UDS_INSUFFICIENT_INDEX_SPACE if the size given will not hold the
 *              index,
 *         UDS_BAD_INDEX_ALIGNMENT if the offset is not zero and the alignment
 *              of the offset is not at an even enough I/O boundary.
 **/
int createSingleFileLayout(IORegion                *region,
                           uint64_t                 offset,
                           uint64_t                 size,
                           const UdsConfiguration   config,
                           IndexLayout            **layoutPtr)
  __attribute__((warn_unused_result));

/**
 * Load a single file layout for an existing index.
 *
 * @param region        The access to the underlying stable storage system;
 *                        the new layout will own the region and close it.
 * @param offset        The offset of the start of the index in bytes as
 *                        specified to createSingleFileLayout().
 * @param layoutPtr     Where to store the layout object for the existing
 *                        index.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int loadSingleFileLayout(IORegion       *region,
                         uint64_t        offset,
                         IndexLayout   **layoutPtr)
  __attribute__((warn_unused_result));

#endif // SINGLE_FILE_LAYOUT_H
