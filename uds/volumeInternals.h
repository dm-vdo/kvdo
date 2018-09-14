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
 * $Id: //eng/uds-releases/gloria/src/uds/volumeInternals.h#4 $
 */

#ifndef VOLUME_INTERNALS_H
#define VOLUME_INTERNALS_H

#include "volume.h"

/* Magic number and versioning */
extern const byte VOLUME_MAGIC_NUMBER[];
extern const byte VOLUME_VERSION[];

extern const unsigned int VOLUME_MAGIC_LENGTH;
extern const unsigned int VOLUME_VERSION_LENGTH;

extern const bool READ_ONLY_VOLUME;

/**
 * Encode the volume format into a buffer.
 *
 * @param volumeFormat The buffer in which to encode the volume format
 * @param geometry     The geometry to encode into the volume format.
 *
 * @return The encoded size of the volume format
 **/
size_t encodeVolumeFormat(byte *volumeFormat, const Geometry *geometry)
  __attribute__((warn_unused_result));

/**
 * Allocate a volume.
 *
 * @param config                The configuration to use
 * @param layout                The index layout
 * @param readQueueMaxSize      The maximum size of the read queue
 * @param zoneCount             The number of zones to use
 * @param readOnly              <code>true</code> if this volume will be
 *                              read-only
 * @param newVolume             A pointer to hold the new volume
 *
 * @return UDS_SUCCESS or an error code
 **/
int allocateVolume(const Configuration  *config,
                   IndexLayout          *layout,
                   unsigned int          readQueueMaxSize,
                   unsigned int          zoneCount,
                   bool                  readOnly,
                   Volume              **newVolume)
  __attribute__((warn_unused_result));

/**
 * Map a chapter number and page number to a phsical volume page number.
 *
 * @param geometry the layout of the volume
 * @param chapter  the chapter number of the desired page
 * @param page     the chapter page number of the desired page
 *
 * @return the physical page number
 **/
int mapToPhysicalPage(Geometry *geometry, int chapter, int page)
  __attribute__((warn_unused_result));

/**
 * Read a page from the volume.
 *
 * @param volume       the volume from which to read the page
 * @param physicalPage the volume page number of the desired page
 * @param buffer       the buffer to hold the page
 *
 * @return UDS_SUCCESS or an error code
 **/
int readPageToBuffer(const Volume *volume,
                     unsigned int  physicalPage,
                     byte         *buffer)
  __attribute__((warn_unused_result));

/**
 * Read a chapter index from the volume.
 *
 * @param volume        the volume from which to read the chapter number
 * @param chapterNumber the volume page number of the desired chapter index
 * @param buffer        the buffer to hold the chapter index
 *
 * @return UDS_SUCCESS or an error code
 **/
int readChapterIndexToBuffer(const Volume *volume,
                             unsigned int  chapterNumber,
                             byte         *buffer)
  __attribute__((warn_unused_result));

#endif /* VOLUME_INTERNALS_H */
