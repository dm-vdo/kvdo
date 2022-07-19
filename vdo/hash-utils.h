/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef HASH_UTILS_H
#define HASH_UTILS_H 1

#include "compiler.h"
#include "common.h"
#include "geometry.h"
#include "numeric.h"
#include "uds.h"

/* How various portions of a chunk name are apportioned. */
enum {
	VOLUME_INDEX_BYTES_OFFSET = 0,
	VOLUME_INDEX_BYTES_COUNT = 8,
	CHAPTER_INDEX_BYTES_OFFSET = 8,
	CHAPTER_INDEX_BYTES_COUNT = 6,
	SAMPLE_BYTES_OFFSET = 14,
	SAMPLE_BYTES_COUNT = 2,
};

static INLINE uint64_t
extract_chapter_index_bytes(const struct uds_chunk_name *name)
{
	const byte *chapter_bits = &name->name[CHAPTER_INDEX_BYTES_OFFSET];
	uint64_t bytes = (uint64_t) get_unaligned_be16(chapter_bits) << 32;

	bytes |= get_unaligned_be32(chapter_bits + 2);
	return bytes;
}

static INLINE uint64_t
extract_volume_index_bytes(const struct uds_chunk_name *name)
{
	return get_unaligned_be64(&name->name[VOLUME_INDEX_BYTES_OFFSET]);
}

static INLINE uint32_t
extract_sampling_bytes(const struct uds_chunk_name *name)
{
	return get_unaligned_be16(&name->name[SAMPLE_BYTES_OFFSET]);
}

/* Compute the chapter delta list for a given name. */
static INLINE unsigned int
hash_to_chapter_delta_list(const struct uds_chunk_name *name,
			   const struct geometry *geometry)
{
	return (unsigned int) ((extract_chapter_index_bytes(name) >>
				geometry->chapter_address_bits) &
			       ((1 << geometry->chapter_delta_list_bits) - 1));
}

/* Compute the chapter delta address for a given name. */
static INLINE unsigned int
hash_to_chapter_delta_address(const struct uds_chunk_name *name,
			      const struct geometry *geometry)
{
	return (unsigned int) (extract_chapter_index_bytes(name) &
			       ((1 << geometry->chapter_address_bits) - 1));
}

static INLINE unsigned int name_to_hash_slot(const struct uds_chunk_name *name,
					     unsigned int slot_count)
{
	return (unsigned int) (extract_chapter_index_bytes(name) % slot_count);
}

unsigned int __must_check compute_bits(unsigned int max_value);

void hash_utils_compile_time_assertions(void);

#endif /* HASH_UTILS_H */
