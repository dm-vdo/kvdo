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
 * $Id: //eng/uds-releases/gloria/src/uds/readOnlyVolume.h#4 $
 */

#ifndef READ_ONLY_VOLUME_H
#define READ_ONLY_VOLUME_H

#include "volume.h"

/**
 * Create a read-only volume.
 *
 * @param config    The configuration to use.
 * @param layout    The layout describing the volume on storage
 * @param newVolume A pointer to hold a pointer to the new volume.
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeReadOnlyVolume(const Configuration  *config,
                       IndexLayout          *layout,
                       Volume              **newVolume)
  __attribute__((warn_unused_result));

/**
 * Retrieve a page from disk into the volume's scratch page.
 *
 * @param volume     The volume containing the page
 * @param chapter    The number of the chapter containing the page
 * @param pageNumber The number of the page
 *
 * @return UDS_SUCCESS or an error code
 **/
int getReadOnlyPage(Volume       *volume,
                    unsigned int  chapter,
                    unsigned int  pageNumber)
  __attribute__((warn_unused_result));

#endif /* READ_ONLY_VOLUME_H */
