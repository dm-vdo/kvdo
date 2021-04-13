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
 * $Id: //eng/uds-releases/krusty/src/uds/nonce.c#11 $
 */

#include "nonce.h"

#include "murmur/MurmurHash3.h"
#include "numeric.h"
#include "random.h"
#include "stringUtils.h"
#include "timeUtils.h"

/*****************************************************************************/
static uint64_t hash_stuff(uint64_t start, const void *data, size_t len)
{
	uint32_t seed = start ^ (start >> 27);
	byte hash_buffer[16];
	MurmurHash3_x64_128(data, len, seed, hash_buffer);
	return get_unaligned_le64(hash_buffer + 4);
}

/*****************************************************************************/
void create_unique_nonce_data(byte *buffer)
{
	ktime_t now = current_time_ns(CLOCK_REALTIME);
	uint32_t rand = random_in_range(1, (1 << 30) - 1);
	size_t offset = 0;

	// Fill NONCE_INFO_SIZE bytes with copies of the time and a
	// pseudorandom number.
	memcpy(buffer + offset, &now, sizeof(now));
	offset += sizeof(now);
	memcpy(buffer + offset, &rand, sizeof(rand));
	offset += sizeof(rand);
	while (offset < NONCE_INFO_SIZE) {
		size_t len = min(NONCE_INFO_SIZE - offset, offset);
		memcpy(buffer + offset, buffer, len);
		offset += len;
	}
}

/*****************************************************************************/
uint64_t generate_primary_nonce(const void *data, size_t len)
{
	return hash_stuff(0xa1b1e0fc, data, len);
}

/*****************************************************************************/
uint64_t generate_secondary_nonce(uint64_t nonce, const void *data, size_t len)
{
	return hash_stuff(nonce + 1, data, len);
}
