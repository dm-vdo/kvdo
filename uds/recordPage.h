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
 * $Id: //eng/uds-releases/gloria/src/uds/recordPage.h#1 $
 */

#ifndef RECORDPAGE_H
#define RECORDPAGE_H 1

#include "common.h"
#include "volume.h"

/**
 * Generate the on-disk encoding of a record page from the list of records
 * in the open chapter representation.
 *
 * @param volume     The volume
 * @param records    The records to be encoded
 *
 * @return UDS_SUCCESS or an error code
 **/
int encodeRecordPage(const Volume *volume, const UdsChunkRecord records[]);

/**
 * Find the metadata for a given block name in this page.
 *
 * @param recordPage The record page
 * @param name       The block name to look for
 * @param geometry   The geometry of the volume
 * @param metadata   an array in which to place the metadata of the
 *                   record, if one was found
 *
 * @return <code>true</code> if the record was found
 **/
bool searchRecordPage(const byte          recordPage[],
                      const UdsChunkName *name,
                      const Geometry     *geometry,
                      UdsChunkData       *metadata);

#endif /* RECORDPAGE_H */
