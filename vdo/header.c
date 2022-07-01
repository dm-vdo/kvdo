// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "header.h"

#include "logger.h"
#include "permassert.h"
#include "status-codes.h"

/**
 * vdo_validate_version() - Check whether a version matches an expected
 *                          version.
 * @expected_version: The expected version.
 * @actual_version: The version being validated.
 * @component_name: The name of the component or the calling function
 *                  (for error logging).
 *
 * Logs an error describing a mismatch.
 *
 * Return: VDO_SUCCESS             if the versions are the same,
 *         VDO_UNSUPPORTED_VERSION if the versions don't match.
 */
int vdo_validate_version(struct version_number expected_version,
			 struct version_number actual_version,
			 const char *component_name)
{
	if (!vdo_are_same_version(expected_version, actual_version)) {
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "%s version mismatch, expected %d.%d, got %d.%d",
					      component_name,
					      expected_version.major_version,
					      expected_version.minor_version,
					      actual_version.major_version,
					      actual_version.minor_version);
	}
	return VDO_SUCCESS;
}

/**
 * vdo_validate_header() - Check whether a header matches expectations.
 * @expected_header: The expected header.
 * @actual_header: The header being validated.
 * @exact_size: If true, the size fields of the two headers must be the same,
 *              otherwise it is required that actual_header.size >=
 *              expected_header.size.
 * @component_name: The name of the component or the calling function
 *                  (for error logging).
 *
 * Logs an error describing the first mismatch found.
 *
 * Return: VDO_SUCCESS             if the header meets expectations,
 *         VDO_INCORRECT_COMPONENT if the component ids don't match,
 *         VDO_UNSUPPORTED_VERSION if the versions or sizes don't match.
 */
int vdo_validate_header(const struct header *expected_header,
			const struct header *actual_header,
			bool exact_size,
			const char *component_name)
{
	int result;

	if (expected_header->id != actual_header->id) {
		return uds_log_error_strerror(VDO_INCORRECT_COMPONENT,
					      "%s ID mismatch, expected %d, got %d",
					      component_name,
					      expected_header->id,
					      actual_header->id);
	}

	result = vdo_validate_version(expected_header->version,
				      actual_header->version, component_name);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if ((expected_header->size > actual_header->size)
	    || (exact_size && (expected_header->size < actual_header->size))) {
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "%s size mismatch, expected %zu, got %zu",
					      component_name,
					      expected_header->size,
					      actual_header->size);
	}

	return VDO_SUCCESS;
}

/**
 * vdo_encode_header() - Encode a header into a buffer.
 * @header: The header to encode.
 * @buffer: The buffer in which to encode the header.
 *
 * Return: UDS_SUCCESS or an error.
 */
int vdo_encode_header(const struct header *header, struct buffer *buffer)
{
	int result;

	if (!ensure_available_space(buffer, VDO_ENCODED_HEADER_SIZE)) {
		return UDS_BUFFER_ERROR;
	}

	result = put_uint32_le_into_buffer(buffer, header->id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = vdo_encode_version_number(header->version, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return put_uint64_le_into_buffer(buffer, header->size);
}

/**
 * vdo_encode_version_number() - Encode a version number into a buffer.
 * @version: The version to encode.
 * @buffer: The buffer in which to encode the version.
 *
 * Return: UDS_SUCCESS or an error.
 */
int vdo_encode_version_number(struct version_number version,
			      struct buffer *buffer)
{
	struct packed_version_number packed = vdo_pack_version_number(version);

	return put_bytes(buffer, sizeof(packed), &packed);
}

/**
 * vdo_decode_header() - Decode a header from a buffer.
 * @buffer: The buffer from which to decode the header.
 * @header: The header structure to decode into.
 *
 * Return: UDS_SUCCESS or an error.
 */
int vdo_decode_header(struct buffer *buffer, struct header *header)
{
	uint32_t id;
	uint64_t size;
	struct version_number version;

	int result = get_uint32_le_from_buffer(buffer, &id);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = vdo_decode_version_number(buffer, &version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &size);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*header = (struct header) {
		.id = id,
		.version = version,
		.size = size,
	};
	return UDS_SUCCESS;
}

/**
 * vdo_decode_version_number() - Decode a version number from a buffer.
 * @buffer: The buffer from which to decode the version.
 * @version: The version structure to decode into.
 *
 * Return: UDS_SUCCESS or an error.
 */
int vdo_decode_version_number(struct buffer *buffer,
			      struct version_number *version)
{
	struct packed_version_number packed;
	int result = get_bytes_from_buffer(buffer, sizeof(packed), &packed);

	if (result != UDS_SUCCESS) {
		return result;
	}

	*version = vdo_unpack_version_number(packed);
	return UDS_SUCCESS;
}
