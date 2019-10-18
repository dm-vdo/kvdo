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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/header.h#2 $
 */

#ifndef HEADER_H
#define HEADER_H

#include "buffer.h"
#include "numeric.h"

#include "types.h"

/**
 * An in-memory representation of a version number for versioned structures on
 * disk.
 *
 * A version number consists of two portions, a major version and a
 * minor version. Any format change which does not require an explicit
 * upgrade step from the previous version should increment the minor
 * version. Any format change which either requires an explicit
 * upgrade step, or is wholly incompatible (i.e. can not be upgraded
 * to), should increment the major version, and set the minor version
 * to 0.
 **/
struct version_number {
  uint32_t majorVersion;
  uint32_t minorVersion;
} __attribute__((packed));

/**
 * A packed, machine-independent, on-disk representation of a version_number.
 * Both fields are stored in little-endian byte order.
 **/
struct packed_version_number {
  byte majorVersion[4];
  byte minorVersion[4];
} __attribute__((packed));

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
struct header {
  ComponentID           id;     // The component this is a header for
  struct version_number version; // The version of the data format
  size_t                size;   // The size of the data following this header
} __attribute__((packed));

enum {
  ENCODED_HEADER_SIZE = sizeof(struct header),
};

/**
 * Check whether two version numbers are the same.
 *
 * @param versionA The first version
 * @param versionB The second version
 *
 * @return <code>true</code> if the two versions are the same
 **/
static inline bool areSameVersion(struct version_number versionA,
                                  struct version_number versionB)
{
  return ((versionA.majorVersion == versionB.majorVersion)
          && (versionA.minorVersion == versionB.minorVersion));
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
static inline bool isUpgradableVersion(struct version_number expectedVersion,
                                       struct version_number actualVersion)
{
  return ((expectedVersion.majorVersion == actualVersion.majorVersion)
          && (expectedVersion.minorVersion > actualVersion.minorVersion));
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
int validateVersion(struct version_number  expectedVersion,
                    struct version_number  actualVersion,
                    const char            *componentName)
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
int validateHeader(const struct header *expectedHeader,
                   const struct header *actualHeader,
                   bool                 exactSize,
                   const char          *componentName)
  __attribute__((warn_unused_result));

/**
 * Encode a header into a buffer.
 *
 * @param header  The header to encode
 * @param buffer  The buffer in which to encode the header
 *
 * @return UDS_SUCCESS or an error
 **/
int encodeHeader(const struct header *header, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Encode a version number into a buffer.
 *
 * @param version  The version to encode
 * @param buffer   The buffer in which to encode the version
 *
 * @return UDS_SUCCESS or an error
 **/
int encodeVersionNumber(struct version_number version, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Decode a header from a buffer.
 *
 * @param [in]  buffer  The buffer from which to decode the header
 * @param [out] header  The header to decode
 *
 * @return UDS_SUCCESS or an error
 **/
int decodeHeader(Buffer *buffer, struct header *header)
  __attribute__((warn_unused_result));

/**
 * Decode a version number from a buffer.
 *
 * @param buffer   The buffer from which to decode the version
 * @param version  The version structure to decode into
 *
 * @return UDS_SUCCESS or an error
 **/
int decodeVersionNumber(Buffer *buffer, struct version_number *version)
  __attribute__((warn_unused_result));

/**
 * Convert a version_number to its packed on-disk representation.
 *
 * @param version  The version number to convert
 *
 * @return the platform-independent representation of the version
 **/
static inline struct packed_version_number
packVersionNumber(struct version_number version)
{
  struct packed_version_number packed;
  storeUInt32LE(packed.majorVersion, version.majorVersion);
  storeUInt32LE(packed.minorVersion, version.minorVersion);
  return packed;
}

/**
 * Convert a packed_version_number to its native in-memory representation.
 *
 * @param version  The version number to convert
 *
 * @return the platform-independent representation of the version
 **/
static inline struct version_number
unpackVersionNumber(struct packed_version_number version)
{
  return (struct version_number) {
    .majorVersion = getUInt32LE(version.majorVersion),
    .minorVersion = getUInt32LE(version.minorVersion),
  };
}

#endif // HEADER_H
