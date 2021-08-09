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
 * $Id: //eng/uds-releases/lisa/src/uds/hashUtils.h#1 $
 */

#ifndef HASH_UTILS_H
#define HASH_UTILS_H 1

#include "compiler.h"
#include "common.h"
#include "geometry.h"
#include "numeric.h"
#include "uds.h"

// How various portions of a hash are apportioned.  Size dependent.
enum {
	VOLUME_INDEX_BYTES_OFFSET = 0, // size 8
	CHAPTER_INDEX_BYTES_OFFSET = 8, // size 6
	SAMPLE_BYTES_OFFSET = 14, // size 2
	VOLUME_INDEX_BYTES_COUNT = 8,
	CHAPTER_INDEX_BYTES_COUNT = 6,
	SAMPLE_BYTES_COUNT = 2,
};

/**
 * Extract the portion of a block name used by the chapter index.
 *
 * @param name The block name
 *
 * @return The chapter index bytes
 **/
static INLINE uint64_t
extract_chapter_index_bytes(const struct uds_chunk_name *name)
{
	// Get the high order 16 bits, then the low order 32 bits
	const byte *chapter_bits = &name->name[CHAPTER_INDEX_BYTES_OFFSET];
	uint64_t bytes = (uint64_t) get_unaligned_be16(chapter_bits) << 32;
	bytes |= get_unaligned_be32(chapter_bits + 2);
	return bytes;
}

/**
 * Extract the portion of a block name used by the volume index.
 *
 * @param name The block name
 *
 * @return The volume index portion of the block name
 **/
static INLINE uint64_t
extract_volume_index_bytes(const struct uds_chunk_name *name)
{
	return get_unaligned_be64(&name->name[VOLUME_INDEX_BYTES_OFFSET]);
}

/**
 * Extract the portion of a block name used for sparse sampling.
 *
 * @param name The block name
 *
 * @return The sparse sample portion of the block name
 **/
static INLINE uint32_t extract_sampling_bytes(const struct uds_chunk_name *name)
{
	return get_unaligned_be16(&name->name[SAMPLE_BYTES_OFFSET]);
}

/**
 * For a given block, find the chapter delta list to use
 *
 * @param name     The block name to hash
 * @param geometry The geometry to use
 *
 * @return The chapter delta list where we expect to find the given blockname
 **/
static INLINE unsigned int
hash_to_chapter_delta_list(const struct uds_chunk_name *name,
			   const struct geometry *geometry)
{
	return (unsigned int) ((extract_chapter_index_bytes(name) >>
				geometry->chapter_address_bits) &
			       ((1 << geometry->chapter_delta_list_bits) - 1));
}

/**
 * For a given block, find the chapter delta address to use
 *
 * @param name     The block name to hash
 * @param geometry The geometry to use
 *
 * @return The chapter delta address to use
 **/
static INLINE unsigned int
hash_to_chapter_delta_address(const struct uds_chunk_name *name,
			      const struct geometry *geometry)
{
	return (unsigned int) (extract_chapter_index_bytes(name) &
			       ((1 << geometry->chapter_address_bits) - 1));
}

/**
 * For a given block name, find the slot in the open chapter hash table
 * where it is expected to reside.
 *
 * @param name        The block name to hash
 * @param slot_count  The size of the hash table
 *
 * @return the record number in the index page where we expect to find
 #         the given blockname
 **/
static INLINE unsigned int name_to_hash_slot(const struct uds_chunk_name *name,
					     unsigned int slot_count)
{
	return (unsigned int) (extract_chapter_index_bytes(name) % slot_count);
}

/**
 * Convert a chunk name to hex to make it more readable.
 *
 * @param chunk_name    The chunk name
 * @param hex_data      The resulting hexdata from the given chunk name
 * @param hex_data_len  The capacity of hex_data
 *
 * @return              UDS_SUCCESS,
 *                      or UDS_INVALID_ARGUMENT if hex_data_len
 *                      is too short.
 **/
int __must_check chunk_name_to_hex(const struct uds_chunk_name *chunk_name,
				   char *hex_data,
				   size_t hex_data_len);

/**
 * Convert chunk data to hex to make it more readable.
 *
 * @param chunk_data    The chunk data
 * @param hex_data      The resulting hexdata from the given chunk data
 * @param hex_data_len  The capacity of hex_data
 *
 * @return              UDS_SUCCESS,
 *                      or UDS_INVALID_ARGUMENT if hex_data_len
 *                      is too short.
 **/
int __must_check chunk_data_to_hex(const struct uds_chunk_data *chunk_data,
				   char *hex_data,
				   size_t hex_data_len);

/**
 * Compute the number of bits required to store a field with the given
 * maximum value.
 *
 * @param max_value  The maximum value of the field
 *
 * @return           the number of bits required
 **/
unsigned int __must_check compute_bits(unsigned int max_value);

/**
 * FOR TESTING. Set the portion of a block name used by the chapter index.
 *
 * @param name   The block name
 * @param value  The value to store
 **/
static INLINE void set_chapter_index_bytes(struct uds_chunk_name *name,
					   uint64_t value)
{
	// Store the high order bytes, then the low-order bytes
	put_unaligned_be16((uint16_t)(value >> 32),
			   &name->name[CHAPTER_INDEX_BYTES_OFFSET]);
	put_unaligned_be32((uint32_t) value,
			   &name->name[CHAPTER_INDEX_BYTES_OFFSET + 2]);
}

/**
 * FOR TESTING. Set the bits used to find a chapter delta list
 *
 * @param name     The block name
 * @param geometry The geometry to use
 * @param value    The value to store
 **/
static INLINE void set_chapter_delta_list_bits(struct uds_chunk_name *name,
					       const struct geometry *geometry,
					       uint64_t value)
{
	uint64_t delta_address = hash_to_chapter_delta_address(name, geometry);
	delta_address |= value << geometry->chapter_address_bits;
	set_chapter_index_bytes(name, delta_address);
}

/**
 * FOR TESTING. Set the portion of a block name used by the volume index.
 *
 * @param name  The block name
 * @param val   The value to store
 **/
static INLINE void set_volume_index_bytes(struct uds_chunk_name *name,
					  uint64_t val)
{
	put_unaligned_be64(val, &name->name[VOLUME_INDEX_BYTES_OFFSET]);
}

/**
 * Set the portion of a block name used for sparse sampling.
 *
 * @param name   The block name
 * @param value  The value to store
 **/
static INLINE void set_sampling_bytes(struct uds_chunk_name *name,
				      uint32_t value)
{
	put_unaligned_be16((uint16_t) value, &name->name[SAMPLE_BYTES_OFFSET]);
}

/**
 * Special function wrapper required for compile-time assertions. This
 * function will fail to compile if UDS_CHUNK_NAME_SIZE is not an integer
 * multiple of 8.
 **/
void hash_utils_compile_time_assertions(void);

#endif /* HASH_UTILS_H */
