/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef HEADER_H
#define HEADER_H

#include "buffer.h"
#include "numeric.h"

#include "types.h"

/*
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
 */
struct version_number {
	uint32_t major_version;
	uint32_t minor_version;
} __packed;

/*
 * A packed, machine-independent, on-disk representation of a version_number.
 * Both fields are stored in little-endian byte order.
 */
struct packed_version_number {
	__le32 major_version;
	__le32 minor_version;
} __packed;

/*
 * The registry of component ids for use in headers
 */
#define VDO_SUPER_BLOCK 0
#define VDO_FIXED_LAYOUT 1
#define VDO_RECOVERY_JOURNAL 2
#define VDO_SLAB_DEPOT 3
#define VDO_BLOCK_MAP 4
#define VDO_GEOMETRY_BLOCK 5

/*
 * The header for versioned data stored on disk.
 */
struct header {
	uint32_t id; /* The component this is a header for */
	struct version_number version; /* The version of the data format */
	size_t size; /* The size of the data following this header */
} __packed;

enum {
	VDO_ENCODED_HEADER_SIZE = sizeof(struct header),
};

/**
 * vdo_are_same_version() - Check whether two version numbers are the same.
 * @version_a: The first version.
 * @version_b: The second version.
 *
 * Return: true if the two versions are the same.
 */
static inline bool vdo_are_same_version(struct version_number version_a,
					struct version_number version_b)
{
	return ((version_a.major_version == version_b.major_version)
		&& (version_a.minor_version == version_b.minor_version));
}

/**
 * vdo_is_upgradable_version() - Check whether an actual version is upgradable
 *                               to an expected version.
 * @expected_version: The expected version.
 * @actual_version: The version being validated.
 *
 * An actual version is upgradable if its major number is expected but its
 * minor number differs, and the expected version's minor number is greater
 * than the actual version's minor number.
 *
 * Return: true if the actual version is upgradable.
 */
static inline bool
vdo_is_upgradable_version(struct version_number expected_version,
			  struct version_number actual_version)
{
	return ((expected_version.major_version == actual_version.major_version)
		&& (expected_version.minor_version > actual_version.minor_version));
}

int __must_check vdo_validate_version(struct version_number expected_version,
				      struct version_number actual_version,
				      const char *component_name);

int __must_check vdo_validate_header(const struct header *expected_header,
				     const struct header *actual_header,
				     bool exact_size,
				     const char *component_name);

int __must_check
vdo_encode_header(const struct header *header, struct buffer *buffer);

int __must_check vdo_encode_version_number(struct version_number version,
					   struct buffer *buffer);

int __must_check vdo_decode_header(struct buffer *buffer,
				   struct header *header);

int __must_check vdo_decode_version_number(struct buffer *buffer,
					   struct version_number *version);

/*
 * Convert a version_number to its packed on-disk representation.
 *
 * @param version  The version number to convert
 *
 * @return the platform-independent representation of the version
 */
static inline struct packed_version_number
vdo_pack_version_number(struct version_number version)
{
	return (struct packed_version_number) {
		.major_version = __cpu_to_le32(version.major_version),
		.minor_version = __cpu_to_le32(version.minor_version),
	};
}

/**
 * vdo_unpack_version_number() - Convert a packed_version_number to its native
 *                               in-memory representation.
 * @version: The version number to convert.
 *
 * Return: The platform-independent representation of the version.
 */
static inline struct version_number
vdo_unpack_version_number(struct packed_version_number version)
{
	return (struct version_number) {
		.major_version = __le32_to_cpu(version.major_version),
		.minor_version = __le32_to_cpu(version.minor_version),
	};
}

#endif /* HEADER_H */
