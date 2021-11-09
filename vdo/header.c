/*
 * Copyright Red Hat
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
 */

#include "header.h"

#include "logger.h"
#include "permassert.h"
#include "status-codes.h"

/**
 * Check whether a version matches an expected version. Logs an error
 * describing a mismatch.
 *
 * @param expected_version  The expected version
 * @param actual_version    The version being validated
 * @param component_name    The name of the component or the calling function
 *                          (for error logging)
 *
 * @return VDO_SUCCESS             if the versions are the same
 *         VDO_UNSUPPORTED_VERSION if the versions don't match
 **/
int validate_vdo_version(struct version_number expected_version,
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
 * Check whether a header matches expectations. Logs an error describing the
 * first mismatch found.
 *
 * @param expected_header  The expected header
 * @param actual_header    The header being validated
 * @param exact_size       If true, the size fields of the two headers must be
 *                         the same, otherwise it is required that
 *                         actual_header.size >= expected_header.size
 * @param component_name   The name of the component or the calling function
 *                         (for error logging)
 *
 * @return VDO_SUCCESS             if the header meets expectations
 *         VDO_INCORRECT_COMPONENT if the component ids don't match
 *         VDO_UNSUPPORTED_VERSION if the versions or sizes don't match
 **/
int validate_vdo_header(const struct header *expected_header,
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

	result = validate_vdo_version(expected_header->version,
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
 * Encode a header into a buffer.
 *
 * @param header  The header to encode
 * @param buffer  The buffer in which to encode the header
 *
 * @return UDS_SUCCESS or an error
 **/
int encode_vdo_header(const struct header *header, struct buffer *buffer)
{
	int result;

	if (!ensure_available_space(buffer, VDO_ENCODED_HEADER_SIZE)) {
		return UDS_BUFFER_ERROR;
	}

	result = put_uint32_le_into_buffer(buffer, header->id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_vdo_version_number(header->version, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return put_uint64_le_into_buffer(buffer, header->size);
}

/**
 * Encode a version number into a buffer.
 *
 * @param version  The version to encode
 * @param buffer   The buffer in which to encode the version
 *
 * @return UDS_SUCCESS or an error
 **/
int encode_vdo_version_number(struct version_number version,
			      struct buffer *buffer)
{
	struct packed_version_number packed = vdo_pack_version_number(version);

	return put_bytes(buffer, sizeof(packed), &packed);
}

/**
 * Decode a header from a buffer.
 *
 * @param [in]  buffer  The buffer from which to decode the header
 * @param [out] header  The header to decode
 *
 * @return UDS_SUCCESS or an error
 **/
int decode_vdo_header(struct buffer *buffer, struct header *header)
{
	uint32_t id;
	uint64_t size;
	struct version_number version;

	int result = get_uint32_le_from_buffer(buffer, &id);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = decode_vdo_version_number(buffer, &version);
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
 * Decode a version number from a buffer.
 *
 * @param buffer   The buffer from which to decode the version
 * @param version  The version structure to decode into
 *
 * @return UDS_SUCCESS or an error
 **/
int decode_vdo_version_number(struct buffer *buffer,
			      struct version_number *version)
{
	struct packed_version_number packed;
	int result = get_bytes_from_buffer(buffer, sizeof(packed), &packed);

	if (result != UDS_SUCCESS) {
		return result;
	}

	*version = unvdo_pack_version_number(packed);
	return UDS_SUCCESS;
}
