/* SPDX-License-Identifier: GPL-2.0-only */
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

#ifndef DELTAINDEX_H
#define DELTAINDEX_H 1

#include "compiler.h"
#include "config.h"
#include "buffered-reader.h"
#include "buffered-writer.h"
#include "compiler.h"
#include "cpu.h"
#include "numeric.h"
#include "time-utils.h"
#include "type-defs.h"

/*
 * This is the largest field size supported by get_field() and set_field().
 * Any field that is larger is not guaranteed to fit in a single byte-aligned
 * uint32_t.
 */
enum {
	MAX_FIELD_BITS = (sizeof(uint32_t) - 1) * CHAR_BIT + 1,
};

/*
 * This is the number of guard bytes needed at the end of the memory byte
 * array when using the bit utilities. 3 bytes are needed when get_field() and
 * set_field() access a field, because they will access some extra bytes past
 * the end of the field. 7 bytes are needed when get_big_field() and
 * set_big_field() access a big field, for the same reason. Note that
 * move_bits() calls get_big_field() and set_big_field(). The definition is
 * written to make it clear how it is derived.
 */
enum {
	POST_FIELD_GUARD_BYTES = sizeof(uint64_t) - 1,
};

/*
 * The maximum size of a single delta list (in bytes). We count guard bytes
 * in this value because a buffer of this size can be used with move_bits().
 */
enum {
	DELTA_LIST_MAX_BYTE_COUNT =
		((UINT16_MAX + CHAR_BIT) / CHAR_BIT + POST_FIELD_GUARD_BYTES)
};

enum {
	/* the number of extra bytes and bits needed to store a collision entry */
	COLLISION_BYTES = UDS_CHUNK_NAME_SIZE,
	COLLISION_BITS = COLLISION_BYTES * CHAR_BIT
};

struct delta_list {
	/* The offset of the delta list start within memory */
	uint64_t start_offset;
	/* The number of bits in the delta list */
	uint16_t size;
	/* Where the last search "found" the key */
	uint16_t save_offset;
	/* The key for the record just before save_offset */
	unsigned int save_key;
};

struct delta_memory {
	/* The delta list memory */
	byte *memory;
	/* The delta list headers */
	struct delta_list *delta_lists;
	/* Temporary starts of delta lists */
	uint64_t *temp_offsets;
	/* Buffered writer for saving an index */
	struct buffered_writer *buffered_writer;
	/* The size of delta list memory */
	size_t size;
	/* Nanoseconds spent rebalancing */
	ktime_t rebalance_time;
	/* Number of memory rebalances */
	int rebalance_count;
	/* The number of bits in a stored value */
	unsigned short value_bits;
	/* The number of bits in the minimal key code */
	unsigned short min_bits;
	/* The number of keys used in a minimal code */
	unsigned int min_keys;
	/* The number of keys used for another code bit */
	unsigned int incr_keys;
	/* The number of records in the index */
	long record_count;
	/* The number of collision records */
	long collision_count;
	/* The number of records removed */
	long discard_count;
	/* The number of UDS_OVERFLOW errors detected */
	long overflow_count;
	/* The index of the first delta list */
	unsigned int first_list;
	/* The number of delta lists */
	unsigned int num_lists;
	/* Tag belonging to this delta index */
	byte tag;
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct delta_list_save_info {
	/* Tag identifying which delta index this list is in */
	uint8_t tag;
	/* Bit offset of the start of the list data */
	uint8_t bit_offset;
	/* Number of bytes of list data */
	uint16_t byte_count;
	/* The delta list number within the delta index */
	uint32_t index;
};

struct delta_index {
	/* The zones */
	struct delta_memory *delta_zones;
	/* The number of zones */
	unsigned int num_zones;
	/* The number of delta lists */
	unsigned int num_lists;
	/* Maximum lists per zone */
	unsigned int lists_per_zone;
	/* The number of non-empty lists at load time per zone */
	unsigned int load_lists[MAX_ZONES];
	/* True if this index is mutable */
	bool is_mutable;
	/* Tag belonging to this delta index */
	byte tag;
};

/*
 * A delta_index_page describes a single page of a chapter index. The
 * delta_index field allows the page to be treated as an immutable delta_index.
 * We use the delta_memory field to treat the chapter index page as a single
 * zone index, and without the need to do an additional memory allocation.
 */

struct delta_index_page {
	struct delta_index delta_index;
	/* These values are loaded from the DeltaPageHeader */
	unsigned int lowest_list_number;
	unsigned int highest_list_number;
	uint64_t virtual_chapter_number;
	/* This structure describes the single zone of a delta index page. */
	struct delta_memory delta_memory;
};

/*
 * Notes on the delta_index_entries:
 *
 * The fields documented as "public" can be read by any code that uses a
 * delta_index. The fields documented as "private" carry information between
 * delta_index method calls and should not be used outside the delta_index
 * module.
 *
 * (1) The delta_index_entry is used like an iterator when searching a delta
 *     list.
 *
 * (2) It is also the result of a successful search and can be used to refer
 *     to the element found by the search.
 *
 * (3) It is also the result of an unsuccessful search and can be used to
 *     refer to the insertion point for a new record.
 *
 * (4) If at_end is true, the delta_list entry can only be used as the
 *     insertion point for a new record at the end of the list.
 *
 * (5) If at_end is false and is_collision is true, the delta_list entry
 *     fields refer to a collision entry in the list, and the delta_list entry
 *     can be used a a reference to this entry.
 *
 * (6) If at_end is false and is_collision is false, the delta_list entry
 *     fields refer to a non-collision entry in the list.  Such delta_list
 *     entries can be used as a reference to a found entry, or an insertion
 *     point for a non-collision entry before this entry, or an insertion
 *     point for a collision entry that collides with this entry.
 */

struct delta_index_entry {
	/* Public fields */
	/* The key for this entry */
	unsigned int key;
	/* We are after the last list entry the list */
	bool at_end;
	/* This record is a collision */
	bool is_collision;

	/* Private fields */
	/* This delta list overflowed */
	bool list_overflow;
	/* The number of bits used for the value */
	unsigned short value_bits;
	/* The number of bits used for the entire entry */
	unsigned short entry_bits;
	/* The delta index zone */
	struct delta_memory *delta_zone;
	/* The delta list containing the entry */
	struct delta_list *delta_list;
	/* The delta list number */
	unsigned int list_number;
	/* Bit offset of this entry within the list */
	uint32_t offset;
	/* The delta between this and previous entry */
	unsigned int delta;
	/* Temporary delta list for immutable indices */
	struct delta_list temp_delta_list;
};

struct delta_index_stats {
	/* Number of bytes allocated */
	size_t memory_allocated;
	/* Nanoseconds spent rebalancing */
	ktime_t rebalance_time;
	/* Number of memory rebalances */
	int rebalance_count;
	/* The number of records in the index */
	long record_count;
	/* The number of collision records */
	long collision_count;
	/* The number of records removed */
	long discard_count;
	/* The number of UDS_OVERFLOW errors detected */
	long overflow_count;
	/* The number of delta lists */
	unsigned int num_lists;
};

/*
 * Get a bit field from a bit stream.
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 *
 * @return the bit field
 */
static INLINE unsigned int
get_field(const byte *memory, uint64_t offset, int size)
{
	const void *addr = memory + offset / CHAR_BIT;

	return ((get_unaligned_le32(addr) >> (offset % CHAR_BIT)) &
		((1 << size) - 1));
}

/*
 * Set a bit field in a bit stream.
 *
 * @param value   The value to put into the field
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 */
static INLINE void
set_field(unsigned int value, byte *memory, uint64_t offset, int size)
{
	void *addr = memory + offset / CHAR_BIT;
	int shift = offset % CHAR_BIT;
	uint32_t data = get_unaligned_le32(addr);

	data &= ~(((1 << size) - 1) << shift);
	data |= value << shift;
	put_unaligned_le32(data, addr);
}

/*
 * Set a bit field in a bit stream to all ones.
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 */
static INLINE void set_one(byte *memory, uint64_t offset, int size)
{
	if (size > 0) {
		byte *addr = memory + offset / CHAR_BIT;
		int shift = offset % CHAR_BIT;
		int count = size + shift > CHAR_BIT ? CHAR_BIT - shift : size;

		*addr++ |= ((1 << count) - 1) << shift;
		for (size -= count; size > CHAR_BIT; size -= CHAR_BIT) {
			*addr++ = 0xFF;
		}

		if (size > 0) {
			*addr |= ~(0xFF << size);
		}
	}
}

/*
 * Set a bit field in a bit stream to all zeros.
 *
 * @param memory  The base memory byte address
 * @param offset  The bit offset into the memory for the start of the field
 * @param size    The number of bits in the field
 */
static INLINE void set_zero(byte *memory, uint64_t offset, int size)
{
	if (size > 0) {
		byte *addr = memory + offset / CHAR_BIT;
		int shift = offset % CHAR_BIT;
		int count = size + shift > CHAR_BIT ? CHAR_BIT - shift : size;

		*addr++ &= ~(((1 << count) - 1) << shift);
		for (size -= count; size > CHAR_BIT; size -= CHAR_BIT) {
			*addr++ = 0;
		}

		if (size > 0) {
			*addr &= 0xFF << size;
		}
	}
}

void get_bytes(const byte *memory,
	       uint64_t offset,
	       byte *destination,
	       int size);

void set_bytes(byte *memory, uint64_t offset, const byte *source, int size);

void move_bits(const byte *s_memory,
	       uint64_t source,
	       byte *d_memory,
	       uint64_t destination,
	       int size);

bool __must_check same_bits(const byte *mem1,
			    uint64_t offset1,
			    const byte *mem2,
			    uint64_t offset2,
			    int size);

int __must_check initialize_delta_memory(struct delta_memory *delta_memory,
					 size_t size,
					 unsigned int first_list,
					 unsigned int num_lists,
					 unsigned int mean_delta,
					 unsigned int num_payload_bits);

void uninitialize_delta_memory(struct delta_memory *delta_memory);

void initialize_delta_memory_page(struct delta_memory *delta_memory,
				  byte *memory,
				  size_t size,
				  unsigned int num_lists,
				  unsigned int mean_delta,
				  unsigned int num_payload_bits);

void empty_delta_lists(struct delta_memory *delta_memory);

int __must_check
start_restoring_delta_memory(struct delta_memory *delta_memory);

void abort_restoring_delta_memory(struct delta_memory *delta_memory);

void start_saving_delta_memory(struct delta_memory *delta_memory,
			       struct buffered_writer *buffered_writer);

int __must_check finish_saving_delta_memory(struct delta_memory *delta_memory);

int __must_check
write_guard_delta_list(struct buffered_writer *buffered_writer);

int __must_check
check_guard_delta_lists(struct buffered_reader **buffered_readers,
			unsigned int num_readers);

int __must_check extend_delta_memory(struct delta_memory *delta_memory,
				     unsigned int growing_index,
				     size_t growing_size,
				     bool do_copy);

size_t get_delta_memory_allocated(const struct delta_memory *delta_memory);

size_t __must_check get_delta_memory_size(unsigned long num_entries,
					  unsigned int mean_delta,
					  unsigned int num_payload_bits);

static INLINE uint64_t
get_delta_list_start(const struct delta_list *delta_list)
{
	return delta_list->start_offset;
}

static INLINE uint16_t
get_delta_list_size(const struct delta_list *delta_list)
{
	return delta_list->size;
}

static INLINE uint64_t get_delta_list_end(const struct delta_list *delta_list)
{
	return get_delta_list_start(delta_list) +
		get_delta_list_size(delta_list);
}

int __must_check initialize_delta_index(struct delta_index *delta_index,
					unsigned int num_zones,
					unsigned int num_lists,
					unsigned int mean_delta,
					unsigned int num_payload_bits,
					size_t memory_size);

int __must_check
initialize_delta_index_page(struct delta_index_page *delta_index_page,
			    uint64_t expected_nonce,
			    unsigned int mean_delta,
			    unsigned int num_payload_bits,
			    byte *memory,
			    size_t mem_size);

void uninitialize_delta_index(struct delta_index *delta_index);

void empty_delta_index(const struct delta_index *delta_index);

void empty_delta_index_zone(const struct delta_index *delta_index,
			    unsigned int zone_number);

int __must_check pack_delta_index_page(const struct delta_index *delta_index,
				       uint64_t header_nonce,
				       byte *memory,
				       size_t mem_size,
				       uint64_t virtual_chapter_number,
				       unsigned int first_list,
				       unsigned int *num_lists);

void set_delta_index_tag(struct delta_index *delta_index, byte tag);

int __must_check
start_restoring_delta_index(struct delta_index *delta_index,
			    struct buffered_reader **buffered_readers,
			    unsigned int num_readers);

int __must_check
finish_restoring_delta_index(struct delta_index *delta_index,
			     struct buffered_reader **buffered_readers,
			     unsigned int num_readers);

void abort_restoring_delta_index(const struct delta_index *delta_index);

int __must_check
start_saving_delta_index(const struct delta_index *delta_index,
			 unsigned int zone_number,
			 struct buffered_writer *buffered_writer);

int __must_check finish_saving_delta_index(const struct delta_index *delta_index,
					   unsigned int zone_number);

size_t __must_check compute_delta_index_save_bytes(unsigned int num_lists,
						   size_t memory_size);

int __must_check start_delta_index_search(const struct delta_index *delta_index,
					  unsigned int list_number,
					  unsigned int key,
					  struct delta_index_entry *iterator);

int __must_check next_delta_index_entry(struct delta_index_entry *delta_entry);

int __must_check
remember_delta_index_offset(const struct delta_index_entry *delta_entry);

int __must_check get_delta_index_entry(const struct delta_index *delta_index,
				       unsigned int list_number,
				       unsigned int key,
				       const byte *name,
				       struct delta_index_entry *delta_entry);

int __must_check
get_delta_entry_collision(const struct delta_index_entry *delta_entry,
			  byte *name);

static INLINE uint64_t
get_delta_entry_offset(const struct delta_index_entry *delta_entry)
{
	return get_delta_list_start(delta_entry->delta_list) +
		delta_entry->offset;
}

static INLINE unsigned int
get_delta_entry_value(const struct delta_index_entry *delta_entry)
{
	return get_field(delta_entry->delta_zone->memory,
			 get_delta_entry_offset(delta_entry),
			 delta_entry->value_bits);
}

int __must_check
set_delta_entry_value(const struct delta_index_entry *delta_entry,
		      unsigned int value);

int __must_check put_delta_index_entry(struct delta_index_entry *delta_entry,
				       unsigned int key,
				       unsigned int value,
				       const byte *name);

int __must_check
remove_delta_index_entry(struct delta_index_entry *delta_entry);

static INLINE unsigned int
get_delta_index_zone(const struct delta_index *delta_index,
		     unsigned int list_number)
{
	return list_number / delta_index->lists_per_zone;
}

unsigned int
get_delta_index_zone_first_list(const struct delta_index *delta_index,
				unsigned int zone_number);

unsigned int
get_delta_index_zone_num_lists(const struct delta_index *delta_index,
			       unsigned int zone_number);

uint64_t __must_check
get_delta_index_zone_dlist_bits_used(const struct delta_index *delta_index,
				     unsigned int zone_number);

uint64_t __must_check
get_delta_index_dlist_bits_allocated(const struct delta_index *delta_index);

void get_delta_index_stats(const struct delta_index *delta_index,
			   struct delta_index_stats *stats);

unsigned int get_delta_index_page_count(unsigned int num_entries,
					unsigned int num_lists,
					unsigned int mean_delta,
					unsigned int num_payload_bits,
					size_t bytes_per_page);

void log_delta_index_entry(struct delta_index_entry *delta_entry);

#endif /* DELTAINDEX_H */
