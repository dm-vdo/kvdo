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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/header.c#4 $
 */

#include "header.h"

#include "logger.h"
#include "permassert.h"
#include "statusCodes.h"

/**********************************************************************/
int validateVersion(VersionNumber  expectedVersion,
                    VersionNumber  actualVersion,
                    const char    *componentName)
{
  if (!areSameVersion(expectedVersion, actualVersion)) {
    return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                                   "%s version mismatch,"
                                   " expected %d.%d, got %d.%d",
                                   componentName,
                                   expectedVersion.majorVersion,
                                   expectedVersion.minorVersion,
                                   actualVersion.majorVersion,
                                   actualVersion.minorVersion);
  }
  return VDO_SUCCESS;
}

/**********************************************************************/
int validateHeader(const Header *expectedHeader,
                   const Header *actualHeader,
                   bool          exactSize,
                   const char   *componentName)
{
  if (expectedHeader->id != actualHeader->id) {
    return logErrorWithStringError(VDO_INCORRECT_COMPONENT,
                                   "%s ID mismatch, expected %d, got %d",
                                   componentName,
                                   expectedHeader->id,
                                   actualHeader->id);
  }

  int result = validateVersion(expectedHeader->version,
                               actualHeader->version,
                               componentName);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if ((expectedHeader->size > actualHeader->size)
      || (exactSize && (expectedHeader->size < actualHeader->size))) {
    return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                                   "%s size mismatch, expected %zu, got %zu",
                                   componentName,
                                   expectedHeader->size,
                                   actualHeader->size);
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int encodeHeader(const Header *header, Buffer *buffer)
{
  if (!ensureAvailableSpace(buffer, ENCODED_HEADER_SIZE)) {
    return UDS_BUFFER_ERROR;
  }

  int result = putUInt32LEIntoBuffer(buffer, header->id);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = encodeVersionNumber(header->version, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return putUInt64LEIntoBuffer(buffer, header->size);
}

/**********************************************************************/
int encodeVersionNumber(VersionNumber version, Buffer *buffer)
{
  PackedVersionNumber packed = packVersionNumber(version);
  return putBytes(buffer, sizeof(packed), &packed);
}

/**********************************************************************/
int decodeHeader(Buffer *buffer, Header *header)
{
  int result = getUInt32LEFromBuffer(buffer, &header->id);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = decodeVersionNumber(buffer, &header->version);
  if (result != UDS_SUCCESS) {
    return result;
  }

  uint64_t size;
  result = getUInt64LEFromBuffer(buffer, &size);
  if (result != UDS_SUCCESS) {
    return result;
  }

  header->size = size;
  return UDS_SUCCESS;
}

/**********************************************************************/
int decodeVersionNumber(Buffer *buffer, VersionNumber *version)
{
  int result = getUInt32LEFromBuffer(buffer, &version->majorVersion);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return getUInt32LEFromBuffer(buffer, &version->minorVersion);
}
