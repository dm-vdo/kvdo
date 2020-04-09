/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/header.c#5 $
 */

#include "header.h"

#include "logger.h"
#include "permassert.h"
#include "statusCodes.h"

/**********************************************************************/
int validate_version(struct version_number expected_version,
		     struct version_number actual_version,
		     const char *component_name)
{
	if (!are_same_version(expected_version, actual_version)) {
		return logErrorWithStringError(
			VDO_UNSUPPORTED_VERSION,
			"%s version mismatch,"
			" expected %d.%d, got %d.%d",
			component_name, expected_version.major_version,
			expected_version.minor_version,
			actual_version.major_version,
			actual_version.minor_version);
	}
	return VDO_SUCCESS;
}

/**********************************************************************/
int validate_header(const struct header *expected_header,
		    const struct header *actual_header,
		    bool exact_size,
		    const char *component_name)
{
	if (expected_header->id != actual_header->id) {
		return logErrorWithStringError(
			VDO_INCORRECT_COMPONENT,
			"%s ID mismatch, expected %d, got %d", component_name,
			expected_header->id, actual_header->id);
	}

	int result = validate_version(expected_header->version,
				      actual_header->version, component_name);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if ((expected_header->size > actual_header->size)
	    || (exact_size && (expected_header->size < actual_header->size))) {
		return logErrorWithStringError(
			VDO_UNSUPPORTED_VERSION,
			"%s size mismatch, expected %zu, got %zu",
			component_name, expected_header->size,
			actual_header->size);
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int encode_header(const struct header *header, Buffer *buffer)
{
	if (!ensure_available_space(buffer, ENCODED_HEADER_SIZE)) {
		return UDS_BUFFER_ERROR;
	}

	int result = put_uint32_le_into_buffer(buffer, header->id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_version_number(header->version, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return put_uint64_le_into_buffer(buffer, header->size);
}

/**********************************************************************/
int encode_version_number(struct version_number version, Buffer *buffer)
{
	struct packed_version_number packed = pack_version_number(version);
	return put_bytes(buffer, sizeof(packed), &packed);
}

/**********************************************************************/
int decode_header(Buffer *buffer, struct header *header)
{
	component_id id;
	int result = get_uint32_le_from_buffer(buffer, &id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	struct version_number version;
	result = decode_version_number(buffer, &version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	uint64_t size;
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

/**********************************************************************/
int decode_version_number(Buffer *buffer, struct version_number *version)
{
	struct packed_version_number packed;
	int result = get_bytes_from_buffer(buffer, sizeof(packed), &packed);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*version = unpack_version_number(packed);
	return UDS_SUCCESS;
}
