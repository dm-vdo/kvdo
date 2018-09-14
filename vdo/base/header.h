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
 * $Id: //eng/vdo-releases/magnesium-rhel7.6/src/c++/vdo/base/header.h#1 $
 */

#ifndef HEADER_H
#define HEADER_H

#include "buffer.h"
#include "types.h"

/**
 * A representation of a version number for versioned structures on disk.
 *
 * A version number consists of two portions, a major version and a
 * minor version. Any format change which does not require an explicit
 * upgrade step from the previous version should increment the minor
 * version. Any format change which either requires an explicit
 * upgrade step, or is wholly incompatible (i.e. can not be upgraded
 * to), should increment the major version, and set the minor version
 * to 0.
 **/
typedef struct {
  uint32_t majorVersion;
  uint32_t minorVersion;
} __attribute__((packed)) VersionNumber;

/**
 * The registry of component ids for use in headers
 **/
typedef enum {
  SUPER_BLOCK       = 0,
  FIXED_LAYOUT      = 1,
  RECOVERY_JOURNAL  = 2,
  SLAB_DEPOT        = 3,
  BLOCK_MAP         = 4,
  GEOMETRY_BLOCK    = 5,
} ComponentID;

/**
 * The header for versioned data stored on disk.
 **/
typedef struct {
  ComponentID         id;       // The component this is a header for
  VersionNumber       version;  // The version of the data format
  size_t              size;     // The size of the data following this header
} __attribute__((packed)) Header;

enum {
  ENCODED_HEADER_SIZE = sizeof(Header),
};

/**
 * Check whether two version numbers are the same.
 *
 * @param versionA The first version
 * @param versionB The second version
 *
 * @return <code>true</code> if the two versions are the same
 **/
static inline bool areSameVersion(const VersionNumber *versionA,
                                  const VersionNumber *versionB)
{
  return ((versionA->majorVersion == versionB->majorVersion)
          && (versionA->minorVersion == versionB->minorVersion));
}

/**
 * Check whether an actual version is upgradable to an expected version.
 * An actual version is upgradable if its major number is expected but
 * its minor number differs, and the expected version's minor number
 * is greater than the actual version's minor number.
 *
 * @param expectedVersion The expected version
 * @param actualVersion   The version being validated
 *
 * @return <code>true</code> if the actual version is upgradable
 **/
static inline bool isUpgradableVersion(const VersionNumber *expectedVersion,
                                       const VersionNumber *actualVersion)
{
  return ((expectedVersion->majorVersion == actualVersion->majorVersion)
          && (expectedVersion->minorVersion > actualVersion->minorVersion));
}

/**
 * Check whether a version matches an expected version. Logs an error
 * describing a mismatch.
 *
 * @param expectedVersion  The expected version
 * @param actualVersion    The version being validated
 * @param componentName    The name of the component or the calling function
 *                         (for error logging)
 *
 * @return VDO_SUCCESS             if the versions are the same
 *         VDO_UNSUPPORTED_VERSION if the versions don't match
 **/
int validateVersion(const VersionNumber *expectedVersion,
                    const VersionNumber *actualVersion,
                    const char          *componentName)
  __attribute__((warn_unused_result));

/**
 * Check whether a header matches expectations. Logs an error describing the
 * first mismatch found.
 *
 * @param expectedHeader  The expected header
 * @param actualHeader    The header being validated
 * @param exactSize       If true, the size fields of the two headers must be
 *                        the same, otherwise actualSize >= expectedSize is OK
 * @param componentName   The name of the component or the calling function
 *                        (for error logging)
 *
 * @return VDO_SUCCESS             if the header meets expectations
 *         VDO_INCORRECT_COMPONENT if the component ids don't match
 *         VDO_UNSUPPORTED_VERSION if the versions or sizes don't match
 **/
int validateHeader(const Header *expectedHeader,
                   const Header *actualHeader,
                   bool          exactSize,
                   const char   *componentName)
  __attribute__((warn_unused_result));

/**
 * Encode a header into a buffer.
 *
 * @param header The header to encode
 * @param buffer The buffer in which to encode the header
 *
 * @return UDS_SUCCESS or an error
 **/
int encodeHeader(const Header *header, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Encode data with a header.
 *
 * @param header The header for the data
 * @param data   The data to encode
 * @param buffer The buffer to hold the encoding
 *
 * @return UDS_SUCCESS or an error
 **/
int encodeWithHeader(const Header *header, const void *data, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Decode a header from a buffer.
 *
 * @param [in]  buffer The buffer from which to decode the header
 * @param [out] header The header to decode
 **/
int decodeHeader(Buffer *buffer, Header *header)
  __attribute__((warn_unused_result));

/**
 * Decode data with a header.
 *
 * @param [in]  buffer The buffer from to decode the data
 * @param [out] header The decoded header
 * @param [out] data   The decoded data
 **/
int decodeWithHeader(Buffer *buffer, Header *header, byte *data)
  __attribute__((warn_unused_result));

#endif // HEADER_H
