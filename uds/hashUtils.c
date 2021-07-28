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
 *
 * $Id: //eng/uds-releases/krusty/src/uds/hashUtils.c#10 $
 */

#include "hashUtils.h"

#include "errors.h"
#include "logger.h"
#include "permassert.h"
#include "stringUtils.h"
#include "uds.h"

/**
 * Convert a byte string to the hex representation.
 *
 * @param data          binary data to convert
 * @param data_len      length of binary data
 * @param hex           target to write hex string into
 * @param hex_len       capacity of target string
 *
 * @return              UDS_SUCCESS,
 *                      or UDS_INVALID_ARGUMENT if hex_len
 *                      is too short.
 **/
static int data_to_hex(const unsigned char *data,
		       size_t data_len,
		       char *hex,
		       size_t hex_len)
{
	size_t i;
	if (hex_len < 2 * data_len + 1) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"hex data incorrect size");
	}
	for (i = 0; i < data_len; ++i) {
		int rc = uds_fixed_sprintf(__func__,
					   &hex[2 * i],
					   hex_len - (2 * i),
					   UDS_INVALID_ARGUMENT,
					   "%02X",
					   data[i]);

		if (rc != UDS_SUCCESS) {
			return rc;
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int chunk_name_to_hex(const struct uds_chunk_name *chunk_name,
		      char *hex_data,
		      size_t hex_data_len)
{
	return data_to_hex(chunk_name->name, UDS_CHUNK_NAME_SIZE,
			   hex_data, hex_data_len);
}

/**********************************************************************/
int chunk_data_to_hex(const struct uds_chunk_data *chunk_data,
		      char *hex_data,
		      size_t hex_data_len)
{
	return data_to_hex(chunk_data->data,
			   UDS_METADATA_SIZE,
			   hex_data,
			   hex_data_len);
}

/**********************************************************************/
unsigned int compute_bits(unsigned int max_value)
{
	// __builtin_clz() counts leading (high-order) zero bits, so if
	// we ever need this to be fast, under GCC we can do:
	// return ((max_value == 0) ? 0 : (32 - __builtin_clz(max_value)));

	unsigned int bits = 0;
	while (max_value > 0) {
		max_value >>= 1;
		bits++;
	}
	return bits;
}

/**********************************************************************/
void hash_utils_compile_time_assertions(void)
{
	STATIC_ASSERT((UDS_CHUNK_NAME_SIZE % sizeof(uint64_t)) == 0);
	STATIC_ASSERT(UDS_CHUNK_NAME_SIZE == 16);
}
