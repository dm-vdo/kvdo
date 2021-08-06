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
 * $Id: //eng/uds-releases/krusty-rhel9.0-beta/src/uds/deltaIndex.c#1 $
 */
#include "deltaIndex.h"

#include "bits.h"
#include "buffer.h"
#include "compiler.h"
#include "cpu.h"
#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "stringUtils.h"
#include "typeDefs.h"
#include "uds.h"
#include "zone.h"

/*
 * A delta index is a key-value store, where each entry maps an address
 * (the key) to a payload (the value).  The entries are sorted by address,
 * and only the delta between successive addresses is stored in the entry.
 * The addresses are assumed to be uniformly distributed,and the deltas are
 * therefore exponentially distributed.
 *
 * The entries could be stored in a single delta_list, but for efficiency we
 * use multiple DeltaLists.  These lists are stored in a single chunk of
 * memory managed by the delta_memory module.  The delta_memory module can
 * move the data around in memory, so we never keep any byte pointers into
 * delta_list memory.  We only keep offsets into the memory.
 *
 * The delta lists are stored as bit streams.  These bit streams are stored
 * in little endian order, and all offsets into delta_memory are bit
 * offsets.
 *
 * All entries are stored as a fixed length payload (the value) followed by a
 * variable length key (the delta). Always strictly in little endian order.
 *
 * A collision entry is used when two block names have the same delta list
 * address.  A collision entry is encoded with DELTA==0, and has 256
 * extension bits containing the full block name.
 *
 * There is a special exception to be noted.  The DELTA==0 encoding usually
 * indicates a collision with the preceding entry.  But for the first entry
 * in any delta list there is no preceding entry, so the DELTA==0 encoding
 * at the beginning of a delta list indicates a normal entry.
 *
 * The Huffman code is driven by 3 parameters:
 *
 *  MINBITS   This is the number of bits in the smallest code
 *
 *  BASE      This is the number of values coded using a code of length MINBITS
 *
 *  INCR      This is the number of values coded by using one additional bit.
 *
 * These parameters are related by:
 *
 *       BASE + INCR == 1 << MINBITS
 *
 * When we create an index, we need to know the mean delta.  From the mean
 * delta, we compute these three parameters.  The math for the Huffman code
 * of an exponential distribution says that we compute:
 *
 *      INCR = log(2) * MEAN_DELTA
 *
 * Then we find the smallest MINBITS so that
 *
 *      1 << MINBITS  >  INCR
 *
 * And then:
 *
 *       BASE = (1 << MINBITS) - INCR
 *
 * Now we need a code such that
 *
 * - The first BASE values code using MINBITS bits
 * - The next INCR values code using MINBITS+1 bits.
 * - The next INCR values code using MINBITS+2 bits.
 * - The next INCR values code using MINBITS+3 bits.
 * - (and so on).
 *
 * ENCODE(DELTA):
 *
 *   if (DELTA < BASE) {
 *       put DELTA in MINBITS bits;
 *   } else {
 *       T1 = (DELTA - BASE) % INCR + BASE;
 *       T2 = (DELTA - BASE) / INCR;
 *       put T1 in MINBITS bits;
 *       put 0 in T2 bits;
 *       put 1 in 1 bit;
 *   }
 *
 * DECODE(BIT_STREAM):
 *
 *   T1 = next MINBITS bits of stream;
 *   if (T1 < BASE) {
 *       DELTA = T1;
 *   } else {
 *       Scan bits in the stream until reading a 1,
 *         setting T2 to the number of 0 bits read;
 *       DELTA = T2 * INCR + T1;
 *   }
 *
 * The bit field utilities that we use on the delta lists assume that it is
 * possible to read a few bytes beyond the end of the bit field.  So we
 * make sure to allocates some extra bytes at the end of memory containing
 * the delta lists.  Look for POST_FIELD_GUARD_BYTES to find the code
 * related to this.
 *
 * And note that the decode bit stream code includes a step that skips over
 * 0 bits until the first 1 bit is found.  A corrupted delta list could
 * cause this step to run off the end of the delta list memory.  As an
 * extra protection against this happening, the guard bytes at the end
 * should be set to all ones.
 */

/**
 * Constants and structures for the saved delta index. "DI" is for
 * delta_index, and -##### is a number to increment when the format of the
 * data changes.
 **/
enum { MAGIC_SIZE = 8 };
static const char MAGIC_DI_START[] = "DI-00002";

struct di_header {
	char magic[MAGIC_SIZE]; // MAGIC_DI_START
	uint32_t zone_number;
	uint32_t num_zones;
	uint32_t first_list;
	uint32_t num_lists;
	uint64_t record_count;
	uint64_t collision_count;
};

//**********************************************************************
//  Methods for dealing with mutable delta list headers
//**********************************************************************

/**
 * Move the start of the delta list bit stream without moving the end.
 *
 * @param delta_list  The delta list header
 * @param increment   The change in the start of the delta list
 **/
static INLINE void move_delta_list_start(struct delta_list *delta_list,
					 int increment)
{
	delta_list->start_offset += increment;
	delta_list->size -= increment;
}

/**
 * Move the end of the delta list bit stream without moving the start.
 *
 * @param delta_list  The delta list header
 * @param increment   The change in the end of the delta list
 **/
static INLINE void move_delta_list_end(struct delta_list *delta_list,
				       int increment)
{
	delta_list->size += increment;
}

//**********************************************************************
//  Methods for dealing with immutable delta list headers packed
//**********************************************************************

// Header data used for immutable delta index pages.  These data are
// followed by the delta list offset table.
struct delta_page_header {
	uint64_t nonce;                   // Externally-defined nonce
	uint64_t virtual_chapter_number;  // The virtual chapter number
	uint16_t first_list;              // Index of the first delta list on
	                                  // the page
	uint16_t num_lists;               // Number of delta lists on the page
} __packed;

// Immutable delta lists are packed into pages containing a header that
// encodes the delta list information into 19 bits per list (64KB bit offset)

enum { IMMUTABLE_HEADER_SIZE = 19 };

/**
 * Get the bit offset to the immutable delta list header
 *
 * @param list_number  The delta list number
 *
 * @return the offset of immutable delta list header
 **/
static INLINE unsigned int get_immutable_header_offset(unsigned int list_number)
{
	return (sizeof(struct delta_page_header) * CHAR_BIT +
		list_number * IMMUTABLE_HEADER_SIZE);
}

/**
 * Get the bit offset to the start of the immutable delta list bit stream
 *
 * @param memory       The memory page containing the delta lists
 * @param list_number  The delta list number
 *
 * @return the start of the delta list
 **/
static INLINE unsigned int get_immutable_start(const byte *memory,
					       unsigned int list_number)
{
	return get_field(memory,
			 get_immutable_header_offset(list_number),
			 IMMUTABLE_HEADER_SIZE);
}

/**
 * Set the bit offset to the start of the immutable delta list bit stream
 *
 * @param memory        The memory page containing the delta lists
 * @param list_number   The delta list number
 * @param start_offset  The start of the delta list
 **/
static INLINE void set_immutable_start(byte *memory,
				       unsigned int list_number,
				       unsigned int start_offset)
{
	set_field(start_offset,
		  memory,
		  get_immutable_header_offset(list_number),
		  IMMUTABLE_HEADER_SIZE);
}

//**********************************************************************
//  Methods for dealing with Delta List Entries
//**********************************************************************

/**
 * Decode a delta index entry delta value. The delta_index_entry basically
 * describes the previous list entry, and has had its offset field changed to
 * point to the subsequent entry. We decode the bit stream and update the
 * DeltaListEntry to describe the entry.
 *
 * @param delta_entry  The delta index entry
 **/
static INLINE void decode_delta(struct delta_index_entry *delta_entry)
{
	int key_bits;
	unsigned int delta;
	const struct delta_memory *delta_zone = delta_entry->delta_zone;
	const byte *memory = delta_zone->memory;
	uint64_t delta_offset =
		get_delta_entry_offset(delta_entry) + delta_entry->value_bits;
	const byte *addr = memory + delta_offset / CHAR_BIT;
	int offset = delta_offset % CHAR_BIT;
	uint32_t data = get_unaligned_le32(addr) >> offset;
	addr += sizeof(uint32_t);
	key_bits = delta_zone->min_bits;
	delta = data & ((1 << key_bits) - 1);
	if (delta >= delta_zone->min_keys) {
		data >>= key_bits;
		if (data == 0) {
			key_bits = sizeof(uint32_t) * CHAR_BIT - offset;
			while ((data = get_unaligned_le32(addr)) == 0) {
				addr += sizeof(uint32_t);
				key_bits += sizeof(uint32_t) * CHAR_BIT;
			}
		}
		key_bits += ffs(data);
		delta += (key_bits - delta_zone->min_bits - 1) *
				delta_zone->incr_keys;
	}
	delta_entry->delta = delta;
	delta_entry->key += delta;

	// Check for a collision, a delta of zero not at the start of the list.
	if (unlikely((delta == 0) && (delta_entry->offset > 0))) {
		delta_entry->is_collision = true;
		// The small duplication of this math in the two arms of this
		// if statement makes a tiny but measurable difference in
		// performance.
		delta_entry->entry_bits =
			delta_entry->value_bits + key_bits + COLLISION_BITS;
	} else {
		delta_entry->is_collision = false;
		delta_entry->entry_bits = delta_entry->value_bits + key_bits;
	}
}

/**
 * Delete bits from a delta list at the offset of the specified delta index
 * entry.
 *
 * @param delta_entry  The delta index entry
 * @param size         The number of bits to delete
 **/
static void delete_bits(const struct delta_index_entry *delta_entry, int size)
{
	uint64_t source, destination;
	uint32_t count;
	bool before_flag;
	struct delta_list *delta_list = delta_entry->delta_list;
	byte *memory = delta_entry->delta_zone->memory;
	// Compute how many bits are retained before and after the deleted bits
	uint32_t total_size = get_delta_list_size(delta_list);
	uint32_t before_size = delta_entry->offset;
	uint32_t after_size = total_size - delta_entry->offset - size;

	// Determine whether to add to the available space either before or
	// after the delta list.  We prefer to move the least amount of data.
	// If it is exactly the same, try to add to the smaller amount of free
	// space.
	if (before_size < after_size) {
		before_flag = true;
	} else if (after_size < before_size) {
		before_flag = false;
	} else {
		uint64_t free_before =
			get_delta_list_start(&delta_list[0]) -
				get_delta_list_end(&delta_list[-1]);
		uint64_t free_after =
			get_delta_list_start(&delta_list[1]) -
				get_delta_list_end(&delta_list[0]);
		before_flag = free_before < free_after;
	}

	if (before_flag) {
		source = get_delta_list_start(delta_list);
		destination = source + size;
		move_delta_list_start(delta_list, size);
		count = before_size;
	} else {
		move_delta_list_end(delta_list, -size);
		destination =
			get_delta_list_start(delta_list) + delta_entry->offset;
		source = destination + size;
		count = after_size;
	}
	move_bits(memory, source, memory, destination, count);
}

/**
 * Get the offset of the collision field in a delta_index_entry
 *
 * @param entry  The delta index record
 *
 * @return the offset of the start of the collision name
 **/
static INLINE uint64_t
get_collision_offset(const struct delta_index_entry *entry)
{
	return (get_delta_entry_offset(entry) + entry->entry_bits -
		COLLISION_BITS);
}

/**
 * Encode a delta index entry delta.
 *
 * @param delta_entry  The delta index entry
 **/
static void encode_delta(const struct delta_index_entry *delta_entry)
{
	unsigned int temp, t1, t2;
	const struct delta_memory *delta_zone = delta_entry->delta_zone;
	byte *memory = delta_zone->memory;
	uint64_t offset =
		get_delta_entry_offset(delta_entry) + delta_entry->value_bits;
	if (delta_entry->delta < delta_zone->min_keys) {
		set_field(delta_entry->delta,
			  memory,
			  offset,
			  delta_zone->min_bits);
		return;
	}
	temp = delta_entry->delta - delta_zone->min_keys;
	t1 = (temp % delta_zone->incr_keys) + delta_zone->min_keys;
	t2 = temp / delta_zone->incr_keys;
	set_field(t1, memory, offset, delta_zone->min_bits);
	set_zero(memory, offset + delta_zone->min_bits, t2);
	set_one(memory, offset + delta_zone->min_bits + t2, 1);
}

/**
 * Encode a delta index entry.
 *
 * @param delta_entry  The delta index entry
 * @param value        The value associated with the entry
 * @param name         For collision entries, the 256 bit full name.
 **/
static void encode_entry(const struct delta_index_entry *delta_entry,
			 unsigned int value,
			 const byte *name)
{
	byte *memory = delta_entry->delta_zone->memory;
	uint64_t offset = get_delta_entry_offset(delta_entry);
	set_field(value, memory, offset, delta_entry->value_bits);
	encode_delta(delta_entry);
	if (name != NULL) {
		set_bytes(memory,
			  get_collision_offset(delta_entry),
			  name,
			  COLLISION_BYTES);
	}
}

/**
 * Insert bits into a delta list at the offset of the specified delta index
 * entry.
 *
 * @param delta_entry  The delta index entry
 * @param size         The number of bits to insert
 *
 * @return UDS_SUCCESS or an error code
 **/
static int insert_bits(struct delta_index_entry *delta_entry, int size)
{
	uint64_t free_before, free_after, source, destination;
	uint32_t count;
	bool before_flag;
	byte *memory;
	struct delta_memory *delta_zone = delta_entry->delta_zone;
	struct delta_list *delta_list = delta_entry->delta_list;
	// Compute how many bits are in use before and after the inserted bits
	uint32_t total_size = get_delta_list_size(delta_list);
	uint32_t before_size = delta_entry->offset;
	uint32_t after_size = total_size - delta_entry->offset;
	if ((unsigned int) (total_size + size) > UINT16_MAX) {
		delta_entry->list_overflow = true;
		delta_zone->overflow_count++;
		return UDS_OVERFLOW;
	}

	// Compute how many bits are available before and after the delta list
	free_before = get_delta_list_start(&delta_list[0]) -
			get_delta_list_end(&delta_list[-1]);
	free_after = get_delta_list_start(&delta_list[1]) -
			get_delta_list_end(&delta_list[0]);

	if (((unsigned int) size <= free_before) &&
	    ((unsigned int) size <= free_after)) {
		// We have enough space to use either before or after the list.
		// Prefer to move the least amount of data.  If it is exactly
		// the same, try to take from the larger amount of free space.
		if (before_size < after_size) {
			before_flag = true;
		} else if (after_size < before_size) {
			before_flag = false;
		} else {
			before_flag = free_before > free_after;
		}
	} else if ((unsigned int) size <= free_before) {
		// There is space before but not after
		before_flag = true;
	} else if ((unsigned int) size <= free_after) {
		// There is space after but not before
		before_flag = false;
	} else {
		// Neither of the surrounding spaces is large enough for this
		// request, Extend and/or rebalance the delta list memory
		// choosing to move the least amount of data.
		int result;
		unsigned int growing_index = delta_entry->list_number + 1;
		before_flag = before_size < after_size;
		if (!before_flag) {
			growing_index++;
		}
		result =
			extend_delta_memory(delta_zone,
					    growing_index,
					    (size + CHAR_BIT - 1) / CHAR_BIT,
					    true);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	if (before_flag) {
		source = get_delta_list_start(delta_list);
		destination = source - size;
		move_delta_list_start(delta_list, -size);
		count = before_size;
	} else {
		move_delta_list_end(delta_list, size);
		source =
			get_delta_list_start(delta_list) + delta_entry->offset;
		destination = source + size;
		count = after_size;
	}
	memory = delta_zone->memory;
	move_bits(memory, source, memory, destination, count);
	return UDS_SUCCESS;
}

/**
 * Get the amount of memory to allocate for each zone
 *
 * @param num_zones    The number of zones in the index
 * @param memory_size  The number of bytes in memory for the index
 *
 * @return the number of bytes to allocate for a single zone
 **/
static INLINE size_t get_zone_memory_size(unsigned int num_zones,
					  size_t memory_size)
{
	size_t zone_size = memory_size / num_zones;
	// Round the size up so that each zone is a multiple of 64K in size.
	enum { ALLOC_BOUNDARY = 64 * KILOBYTE };
	return (zone_size + ALLOC_BOUNDARY - 1) & -ALLOC_BOUNDARY;
}

/**
 * Validate delta index parameters
 *
 * @param mean_delta        The mean delta value
 * @param num_payload_bits  The number of bits in the payload or value
 **/
static bool invalid_parameters(unsigned int mean_delta,
			       unsigned int num_payload_bits)
{
	const unsigned int min_delta = 10;
	const unsigned int max_delta = 1 << MAX_FIELD_BITS;
	if ((mean_delta < min_delta) || (mean_delta > max_delta)) {
		uds_log_warning("error initializing delta index: mean delta (%u) is not in the range %u to %u",
				mean_delta,
				min_delta,
				max_delta);
		return true;
	}
	if (num_payload_bits > MAX_FIELD_BITS) {
		uds_log_warning("error initializing delta index: Too many payload bits (%u)",
				num_payload_bits);
		return true;
	}
	return false;
}

/**
 * Set a delta index entry to be a collision
 *
 * @param delta_entry  The delta index entry
 **/
static void set_collision(struct delta_index_entry *delta_entry)
{
	delta_entry->is_collision = true;
	delta_entry->entry_bits += COLLISION_BITS;
}

/**
 * Set the delta in a delta index entry.
 *
 * @param delta_entry  The delta index entry
 * @param delta        The new delta
 **/
static void set_delta(struct delta_index_entry *delta_entry, unsigned int delta)
{
	const struct delta_memory *delta_zone = delta_entry->delta_zone;
	int key_bits = delta_zone->min_bits +
				((delta_zone->incr_keys -
					delta_zone->min_keys + delta) /
						delta_zone->incr_keys);
	delta_entry->delta = delta;
	delta_entry->entry_bits = delta_entry->value_bits + key_bits;
}

//**********************************************************************
//  External functions declared in delta_index.h
//**********************************************************************

int initialize_delta_index(struct delta_index *delta_index,
			   unsigned int num_zones,
			   unsigned int num_lists,
			   unsigned int mean_delta,
			   unsigned int num_payload_bits,
			   size_t memory_size)
{
	int result;
	unsigned int z;
	size_t mem_size = get_zone_memory_size(num_zones, memory_size);
	if (invalid_parameters(mean_delta, num_payload_bits)) {
		return UDS_INVALID_ARGUMENT;
	}

	result = UDS_ALLOCATE(num_zones,
			      struct delta_memory,
			      "Delta Index Zones",
			      &delta_index->delta_zones);
	if (result != UDS_SUCCESS) {
		return result;
	}

	delta_index->num_zones = num_zones;
	delta_index->num_lists = num_lists;
	delta_index->lists_per_zone = (num_lists + num_zones - 1) / num_zones;
	delta_index->is_mutable = true;
	delta_index->tag = 'm';

	for (z = 0; z < num_zones; z++) {
		unsigned int first_list_in_zone =
			z * delta_index->lists_per_zone;
		unsigned int num_lists_in_zone = delta_index->lists_per_zone;
		if (z == num_zones - 1) {
			/*
			 * The last zone gets fewer lists if num_zones doesn't
			 * evenly divide num_lists. We'll have an underflow if
			 * the assertion below doesn't hold. (And it turns out
			 * that the assertion is equivalent to num_zones <= 1 +
			 * (num_lists / num_zones) + (num_lists % num_zones) in
			 * the case that num_zones doesn't evenly divide
			 * numlists. If num_lists >= num_zones * num_zones,
			 * then the above inequality will always hold.)
			 */
			if (delta_index->num_lists <= first_list_in_zone) {
				uninitialize_delta_index(delta_index);
				return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
							      "%u delta-lists not enough for %u zones",
							      num_lists,
							      num_zones);
			}
			num_lists_in_zone =
				delta_index->num_lists - first_list_in_zone;
		}
		result = initialize_delta_memory(&delta_index->delta_zones[z],
						 mem_size,
						 first_list_in_zone,
						 num_lists_in_zone,
						 mean_delta,
						 num_payload_bits);
		if (result != UDS_SUCCESS) {
			uninitialize_delta_index(delta_index);
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
static bool verify_delta_index_page(uint64_t nonce,
				    uint16_t num_lists,
				    uint64_t expected_nonce,
				    byte *memory,
				    size_t mem_size)
{
	unsigned int i;
	// Verify the nonce.  A mismatch here happens in normal operation when
	// we are doing a rebuild but haven't written the entire volume once.
	if (nonce != expected_nonce) {
		return false;
	}

	// Verify that the number of delta lists can fit in the page.
	if (num_lists > (mem_size - sizeof(struct delta_page_header)) *
				CHAR_BIT / IMMUTABLE_HEADER_SIZE) {
		return false;
	}

	// Verify that the first delta list is immediately after the last delta
	// list header.
	if (get_immutable_start(memory, 0) !=
	    get_immutable_header_offset(num_lists + 1)) {
		return false;
	}

	// Verify that the lists are in the correct order.
	for (i = 0; i < num_lists; i++) {
		if (get_immutable_start(memory, i) >
		    get_immutable_start(memory, i + 1)) {
			return false;
		}
	}

	// Verify that the last list ends on the page, and that there is room
	// for the post-field guard bits.
	if (get_immutable_start(memory, num_lists) >
	    (mem_size - POST_FIELD_GUARD_BYTES) * CHAR_BIT) {
		return false;
	}

	// Verify that the guard bytes are correctly set to all ones.
	for (i = 0; i < POST_FIELD_GUARD_BYTES; i++) {
		byte guard_byte = memory[mem_size - POST_FIELD_GUARD_BYTES + i];
		if (guard_byte != (byte) ~0) {
			return false;
		}
	}

	// All verifications passed.
	return true;
}

/**********************************************************************/
int initialize_delta_index_page(struct delta_index_page *delta_index_page,
				uint64_t expected_nonce,
				unsigned int mean_delta,
				unsigned int num_payload_bits,
				byte *memory,
				size_t mem_size)
{
	uint64_t nonce, vcn, first_list, num_lists;
	const struct delta_page_header *header =
		(const struct delta_page_header *) memory;

	if (invalid_parameters(mean_delta, num_payload_bits)) {
		return UDS_INVALID_ARGUMENT;
	}

	// First assume that the header is little endian
	nonce = get_unaligned_le64((const byte *) &header->nonce);
	vcn = get_unaligned_le64((const byte *) &header->virtual_chapter_number);
	first_list = get_unaligned_le16((const byte *) &header->first_list);
	num_lists = get_unaligned_le16((const byte *) &header->num_lists);
	if (!verify_delta_index_page(nonce, num_lists, expected_nonce, memory,
				     mem_size)) {
		// That failed, so try big endian
		nonce = get_unaligned_be64((const byte *) &header->nonce);
		vcn = get_unaligned_be64((const byte *) &header->virtual_chapter_number);
		first_list =
			get_unaligned_be16((const byte *) &header->first_list);
		num_lists =
			get_unaligned_be16((const byte *) &header->num_lists);
		if (!verify_delta_index_page(nonce,
					     num_lists,
					     expected_nonce,
					     memory,
					     mem_size)) {
			// Also failed.  Do not log this as an error.  It
			// happens in normal operation when we are doing a
			// rebuild but haven't written the entire volume once.
			return UDS_CORRUPT_COMPONENT;
		}
	}

	delta_index_page->delta_index.delta_zones =
		&delta_index_page->delta_memory;
	delta_index_page->delta_index.num_zones = 1;
	delta_index_page->delta_index.num_lists = num_lists;
	delta_index_page->delta_index.lists_per_zone = num_lists;
	delta_index_page->delta_index.is_mutable = false;
	delta_index_page->delta_index.tag = 'p';
	delta_index_page->virtual_chapter_number = vcn;
	delta_index_page->lowest_list_number = first_list;
	delta_index_page->highest_list_number = first_list + num_lists - 1;

	initialize_delta_memory_page(&delta_index_page->delta_memory,
				     (byte *) memory,
				     mem_size,
				     num_lists,
				     mean_delta,
				     num_payload_bits);
	return UDS_SUCCESS;
}

/**********************************************************************/
void uninitialize_delta_index(struct delta_index *delta_index)
{
	if (delta_index != NULL) {
		unsigned int z;
		for (z = 0; z < delta_index->num_zones; z++) {
			uninitialize_delta_memory(&delta_index->delta_zones[z]);
		}
		UDS_FREE(delta_index->delta_zones);
		memset(delta_index, 0, sizeof(struct delta_index));
	}
}

/**********************************************************************/
void empty_delta_index(const struct delta_index *delta_index)
{
	unsigned int z;
	for (z = 0; z < delta_index->num_zones; z++) {
		empty_delta_lists(&delta_index->delta_zones[z]);
	}
}

/**********************************************************************/
void empty_delta_index_zone(const struct delta_index *delta_index,
			    unsigned int zone_number)
{
	empty_delta_lists(&delta_index->delta_zones[zone_number]);
}

/**********************************************************************/
int pack_delta_index_page(const struct delta_index *delta_index,
			  uint64_t header_nonce,
			  byte *memory,
			  size_t mem_size,
			  uint64_t virtual_chapter_number,
			  unsigned int first_list,
			  unsigned int *num_lists)
{
	const struct delta_memory *delta_zone;
	struct delta_list *delta_lists;
	unsigned int max_lists, n_lists = 0, offset, i;
	int num_bits;
	struct delta_page_header *header;
	if (!delta_index->is_mutable) {
		return uds_log_error_strerror(UDS_BAD_STATE,
					      "Cannot pack an immutable index");
	}
	if (delta_index->num_zones != 1) {
		return uds_log_error_strerror(UDS_BAD_STATE,
					      "Cannot pack a delta index page when the index has %u zones",
					      delta_index->num_zones);
	}
	if (first_list > delta_index->num_lists) {
		return uds_log_error_strerror(UDS_BAD_STATE,
					      "Cannot pack a delta index page when the first list (%u) is larger than the number of lists (%u)",
					      first_list,
					      delta_index->num_lists);
	}

	delta_zone = &delta_index->delta_zones[0];
	delta_lists =
		&delta_zone->delta_lists[first_list + 1];
	max_lists = delta_index->num_lists - first_list;

	// Compute how many lists will fit on the page
	num_bits = mem_size * CHAR_BIT;
	// Subtract the size of the fixed header and 1 delta list offset
	num_bits -= get_immutable_header_offset(1);
	// Subtract the guard bytes of memory so that allow us to freely read a
	// short distance past the end of any byte we are interested in.
	num_bits -= POST_FIELD_GUARD_BYTES * CHAR_BIT;
	if (num_bits < IMMUTABLE_HEADER_SIZE) {
		// This page is too small to contain even one empty delta list
		return uds_log_error_strerror(UDS_OVERFLOW,
					      "Chapter Index Page of %zu bytes is too small",
					      mem_size);
	}

	while (n_lists < max_lists) {
		// Each list requires 1 delta list offset and the list data
		int bits = IMMUTABLE_HEADER_SIZE +
			   get_delta_list_size(&delta_lists[n_lists]);
		if (bits > num_bits) {
			break;
		}
		n_lists++;
		num_bits -= bits;
	}
	*num_lists = n_lists;

	// Construct the page header
	header = (struct delta_page_header *) memory;
	put_unaligned_le64(header_nonce, (byte *) &header->nonce);
	put_unaligned_le64(virtual_chapter_number,
			   (byte *) &header->virtual_chapter_number);
	put_unaligned_le16(first_list, (byte *) &header->first_list);
	put_unaligned_le16(n_lists, (byte *) &header->num_lists);

	// Construct the delta list offset table, making sure that the memory
	// page is large enough.
	offset = get_immutable_header_offset(n_lists + 1);
	set_immutable_start(memory, 0, offset);
	for (i = 0; i < n_lists; i++) {
		offset += get_delta_list_size(&delta_lists[i]);
		set_immutable_start(memory, i + 1, offset);
	}

	// Copy the delta list data onto the memory page
	for (i = 0; i < n_lists; i++) {
		struct delta_list *delta_list = &delta_lists[i];
		move_bits(delta_zone->memory,
			  get_delta_list_start(delta_list),
			  memory,
			  get_immutable_start(memory, i),
			  get_delta_list_size(delta_list));
	}

	// Set all the bits in the guard bytes.  Do not use the bit field
	// utilities.
	memset(memory + mem_size - POST_FIELD_GUARD_BYTES,
	       ~0,
	       POST_FIELD_GUARD_BYTES);
	return UDS_SUCCESS;
}


/**********************************************************************/
void set_delta_index_tag(struct delta_index *delta_index, byte tag)
{
	unsigned int z;
	delta_index->tag = tag;
	for (z = 0; z < delta_index->num_zones; z++) {
		delta_index->delta_zones[z].tag = tag;
	}
}

/**********************************************************************/
static int __must_check decode_delta_index_header(struct buffer *buffer,
						  struct di_header *header)
{
	int result = get_bytes_from_buffer(buffer, MAGIC_SIZE, &header->magic);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &header->zone_number);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &header->num_zones);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &header->first_list);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &header->num_lists);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &header->record_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &header->collision_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
				 "%zu bytes decoded of %zu expected",
				 buffer_length(buffer) - content_length(buffer),
				 buffer_length(buffer));
	return result;
}

/**********************************************************************/
static int __must_check read_delta_index_header(struct buffered_reader *reader,
						struct di_header *header)
{
	struct buffer *buffer;

	int result = make_buffer(sizeof(*header), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_from_buffered_reader(reader, get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return uds_log_warning_strerror(result,
						"failed to read delta index header");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = decode_delta_index_header(buffer, header);
	free_buffer(UDS_FORGET(buffer));
	return result;
}

/**********************************************************************/
int start_restoring_delta_index(const struct delta_index *delta_index,
				struct buffered_reader   **buffered_readers,
				int num_readers)
{
	unsigned int num_zones = num_readers;
	unsigned long record_count = 0, collision_count = 0;
	unsigned int first_list[MAX_ZONES], num_lists[MAX_ZONES];
	struct buffered_reader *reader[MAX_ZONES];
	unsigned int z, list_next = 0;
	bool zone_flags[MAX_ZONES] = {
		false,
	};

	if (!delta_index->is_mutable) {
		return uds_log_error_strerror(UDS_BAD_STATE,
					      "Cannot restore to an immutable index");
	}
	if (num_readers <= 0) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"No delta index files");
	}

	if (num_zones > MAX_ZONES) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "zone count %u must not exceed MAX_ZONES",
					      num_zones);
	}

	// Read the header from each file, and make sure we have a matching set
	for (z = 0; z < num_zones; z++) {
		struct di_header header;
		int result =
			read_delta_index_header(buffered_readers[z], &header);
		if (result != UDS_SUCCESS) {
			return uds_log_warning_strerror(result,
							"failed to read delta index header");
		}
		if (memcmp(header.magic, MAGIC_DI_START, MAGIC_SIZE) != 0) {
			return uds_log_warning_strerror(UDS_CORRUPT_COMPONENT,
							"delta index file has bad magic number");
		}
		if (num_zones != header.num_zones) {
			return uds_log_warning_strerror(UDS_CORRUPT_COMPONENT,
							"delta index files contain mismatched zone counts (%u,%u)",
							num_zones,
							header.num_zones);
		}
		if (header.zone_number >= num_zones) {
			return uds_log_warning_strerror(UDS_CORRUPT_COMPONENT,
							"delta index files contains zone %u of %u zones",
							header.zone_number,
							num_zones);
		}
		if (zone_flags[header.zone_number]) {
			return uds_log_warning_strerror(UDS_CORRUPT_COMPONENT,
							"delta index files contain two of zone %u",
							header.zone_number);
		}
		reader[header.zone_number] = buffered_readers[z];
		first_list[header.zone_number] = header.first_list;
		num_lists[header.zone_number] = header.num_lists;
		zone_flags[header.zone_number] = true;
		record_count += header.record_count;
		collision_count += header.collision_count;
	}
	for (z = 0; z < num_zones; z++) {
		if (first_list[z] != list_next) {
			return uds_log_warning_strerror(UDS_CORRUPT_COMPONENT,
							"delta index file for zone %u starts with list %u instead of list %u",
							z,
							first_list[z],
							list_next);
		}
		list_next += num_lists[z];
	}
	if (list_next != delta_index->num_lists) {
		return uds_log_warning_strerror(UDS_CORRUPT_COMPONENT,
						"delta index files contain %u delta lists instead of %u delta lists",
						list_next,
						delta_index->num_lists);
	}
	if (collision_count > record_count) {
		return uds_log_warning_strerror(UDS_CORRUPT_COMPONENT,
						"delta index files contain %ld collisions and %ld records",
						collision_count,
						record_count);
	}

	empty_delta_index(delta_index);
	delta_index->delta_zones[0].record_count = record_count;
	delta_index->delta_zones[0].collision_count = collision_count;

	// Read the delta list sizes from the files, and distribute each of
	// them to proper zone
	for (z = 0; z < num_zones; z++) {
		unsigned int i;
		for (i = 0; i < num_lists[z]; i++) {
			uint16_t delta_list_size;
			unsigned int list_number, zone_number;
			const struct delta_memory *delta_zone;
			byte delta_list_size_data[sizeof(uint16_t)];
			int result =
				read_from_buffered_reader(reader[z],
							  delta_list_size_data,
							  sizeof(delta_list_size_data));
			if (result != UDS_SUCCESS) {
				return uds_log_warning_strerror(result,
								"failed to read delta index size");
			}
			delta_list_size = get_unaligned_le16(delta_list_size_data);
			list_number = first_list[z] + i;
			zone_number = get_delta_index_zone(delta_index, list_number);
			delta_zone = &delta_index->delta_zones[zone_number];
			list_number -= delta_zone->first_list;
			delta_zone->delta_lists[list_number + 1].size =
				delta_list_size;
		}
	}

	// Prepare each zone to start receiving the delta list data
	for (z = 0; z < delta_index->num_zones; z++) {
		int result =
			start_restoring_delta_memory(&delta_index->delta_zones[z]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
bool is_restoring_delta_index_done(const struct delta_index *delta_index)
{
	unsigned int z;
	for (z = 0; z < delta_index->num_zones; z++) {
		if (!are_delta_memory_transfers_done(&delta_index->delta_zones[z])) {
			return false;
		}
	}
	return true;
}

/**********************************************************************/
int restore_delta_list_to_delta_index(const struct delta_index *delta_index,
				      const struct delta_list_save_info *dlsi,
				      const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
	unsigned int zone_number;
	// Make sure the data are intended for this delta list.  Do not
	// log an error, as this may be valid data for another delta index.
	if (dlsi->tag != delta_index->tag) {
		return UDS_CORRUPT_COMPONENT;
	}

	if (dlsi->index >= delta_index->num_lists) {
		return uds_log_warning_strerror(UDS_CORRUPT_COMPONENT,
						"invalid delta list number %u of %u",
						dlsi->index,
						delta_index->num_lists);
	}

	zone_number = get_delta_index_zone(delta_index, dlsi->index);
	return restore_delta_list(&delta_index->delta_zones[zone_number],
				  dlsi, data);
}

/**********************************************************************/
void abort_restoring_delta_index(const struct delta_index *delta_index)
{
	unsigned int z;
	for (z = 0; z < delta_index->num_zones; z++) {
		abort_restoring_delta_memory(&delta_index->delta_zones[z]);
	}
}

/**********************************************************************/
static int __must_check encode_delta_index_header(struct buffer *buffer,
						  struct di_header *header)
{
	int result = put_bytes(buffer, MAGIC_SIZE, MAGIC_DI_START);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, header->zone_number);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, header->num_zones);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, header->first_list);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, header->num_lists);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, header->record_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, header->collision_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) == sizeof(*header),
				 "%zu bytes encoded of %zu expected",
				 content_length(buffer),
				 sizeof(*header));

	return result;
}

/**********************************************************************/
int start_saving_delta_index(const struct delta_index *delta_index,
			     unsigned int zone_number,
			     struct buffered_writer *buffered_writer)
{
	struct buffer *buffer;
	int result;
	unsigned int i;
	struct delta_memory *delta_zone =
		&delta_index->delta_zones[zone_number];
	struct di_header header;
	memcpy(header.magic, MAGIC_DI_START, MAGIC_SIZE);
	header.zone_number = zone_number;
	header.num_zones = delta_index->num_zones;
	header.first_list = delta_zone->first_list;
	header.num_lists = delta_zone->num_lists;
	header.record_count = delta_zone->record_count;
	header.collision_count = delta_zone->collision_count;

	result = make_buffer(sizeof(struct di_header), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_delta_index_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return uds_log_warning_strerror(result,
						"failed to write delta index header");
	}

	for (i = 0; i < delta_zone->num_lists; i++) {
		uint16_t delta_list_size =
			get_delta_list_size(&delta_zone->delta_lists[i + 1]);
		byte data[2];
		put_unaligned_le16(delta_list_size, data);
		result = write_to_buffered_writer(buffered_writer, data,
						  sizeof(data));
		if (result != UDS_SUCCESS) {
			return uds_log_warning_strerror(result,
							"failed to write delta list size");
		}
	}

	start_saving_delta_memory(delta_zone, buffered_writer);
	return UDS_SUCCESS;
}

/**********************************************************************/
bool is_saving_delta_index_done(const struct delta_index *delta_index,
				unsigned int zone_number)
{
	return are_delta_memory_transfers_done(&delta_index->delta_zones[zone_number]);
}

/**********************************************************************/
int finish_saving_delta_index(const struct delta_index *delta_index,
			      unsigned int zone_number)
{
	return finish_saving_delta_memory(&delta_index->delta_zones[zone_number]);
}

/**********************************************************************/
int abort_saving_delta_index(const struct delta_index *delta_index,
			     unsigned int zone_number)
{
	abort_saving_delta_memory(&delta_index->delta_zones[zone_number]);
	return UDS_SUCCESS;
}

/**********************************************************************/
size_t compute_delta_index_save_bytes(unsigned int num_lists,
				      size_t memory_size)
{
	// The exact amount of memory used depends upon the number of zones.
	// Compute the maximum potential memory size.
	size_t max_mem_size = memory_size;
	unsigned int num_zones;
	for (num_zones = 1; num_zones <= MAX_ZONES; num_zones++) {
		size_t mem_size = get_zone_memory_size(num_zones, memory_size);
		if (mem_size > max_mem_size) {
			max_mem_size = mem_size;
		}
	}
	// Saving a delta index requires a header ...
	return (sizeof(struct di_header)
		// ... plus a delta_list_save_info per delta list
		// plus an extra byte per delta list ...
		+ num_lists * (sizeof(struct delta_list_save_info) + 1)
		// ... plus the delta list memory
		+ max_mem_size);
}

/**********************************************************************/
int validate_delta_index(const struct delta_index *delta_index)
{
	unsigned int z;
	for (z = 0; z < delta_index->num_zones; z++) {
		int result =
			validate_delta_lists(&delta_index->delta_zones[z]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
static int assert_not_at_end(const struct delta_index_entry *delta_entry,
			     int error_code)
{
	return ASSERT_WITH_ERROR_CODE(!delta_entry->at_end, error_code,
		                      "operation is invalid because the list entry is at the end of the delta list");
}

/**********************************************************************/
static void prefetch_delta_list(const struct delta_memory *delta_zone,
				const struct delta_list *delta_list)
{
	const byte *memory = delta_zone->memory;
	const byte *addr =
		&memory[get_delta_list_start(delta_list) / CHAR_BIT];
	unsigned int size = get_delta_list_size(delta_list) / CHAR_BIT;
	prefetch_range(addr, size, false);
}

/**********************************************************************/
int start_delta_index_search(const struct delta_index *delta_index,
			     unsigned int list_number,
			     unsigned int key,
			     bool read_only,
			     struct delta_index_entry *delta_entry)
{
	unsigned int zone_number;
	struct delta_memory *delta_zone;
	struct delta_list *delta_list;
	int result = ASSERT_WITH_ERROR_CODE((list_number <
					     delta_index->num_lists),
					    UDS_CORRUPT_DATA,
					    "Delta list number (%u) is out of range (%u)",
					    list_number,
					    delta_index->num_lists);
	if (result != UDS_SUCCESS) {
		return result;
	}

	zone_number = get_delta_index_zone(delta_index, list_number);
	delta_zone = &delta_index->delta_zones[zone_number];
	list_number -= delta_zone->first_list;
	result = ASSERT_WITH_ERROR_CODE((list_number < delta_zone->num_lists),
					UDS_CORRUPT_DATA,
					"Delta list number (%u)"
					" is out of range (%u) for zone (%u)",
					list_number,
					delta_zone->num_lists,
					zone_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (delta_index->is_mutable) {
		delta_list = &delta_zone->delta_lists[list_number + 1];
		if (!read_only) {
			// Here is the lazy writing of the index for a
			// checkpoint
			lazy_flush_delta_list(delta_zone, list_number);
		}
	} else {
		unsigned int end_offset;
		// Translate the immutable delta list header into a temporary
		// full delta list header
		delta_list = &delta_entry->temp_delta_list;
		delta_list->start_offset =
			get_immutable_start(delta_zone->memory, list_number);
		end_offset = get_immutable_start(delta_zone->memory,
					         list_number + 1);
		delta_list->size = end_offset - delta_list->start_offset;
		delta_list->save_key = 0;
		delta_list->save_offset = 0;
	}

	if (key > delta_list->save_key) {
		delta_entry->key = delta_list->save_key;
		delta_entry->offset = delta_list->save_offset;
	} else {
		delta_entry->key = 0;
		delta_entry->offset = 0;
		if (key == 0) {
			// This usually means we're about to walk the entire
			// delta list, so get all of it into the CPU cache.
			prefetch_delta_list(delta_zone, delta_list);
		}
	}

	delta_entry->at_end = false;
	delta_entry->delta_zone = delta_zone;
	delta_entry->delta_list = delta_list;
	delta_entry->entry_bits = 0;
	delta_entry->is_collision = false;
	delta_entry->list_number = list_number;
	delta_entry->list_overflow = false;
	delta_entry->value_bits = delta_zone->value_bits;
	return UDS_SUCCESS;
}

/**********************************************************************/
noinline int next_delta_index_entry(struct delta_index_entry *delta_entry)
{
	const struct delta_list *delta_list;
	unsigned int next_offset, size;
	int result = assert_not_at_end(delta_entry, UDS_BAD_STATE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	delta_list = delta_entry->delta_list;
	delta_entry->offset += delta_entry->entry_bits;
	size = get_delta_list_size(delta_list);
	if (unlikely(delta_entry->offset >= size)) {
		delta_entry->at_end = true;
		delta_entry->delta = 0;
		delta_entry->is_collision = false;
		return ASSERT_WITH_ERROR_CODE((delta_entry->offset == size),
					      UDS_CORRUPT_DATA,
					      "next offset past end of delta list");
	}

	decode_delta(delta_entry);

	next_offset = delta_entry->offset + delta_entry->entry_bits;
	if (next_offset > size) {
		// This is not an assertion because
		// validate_chapter_index_page() wants to handle this error.
		uds_log_warning("Decoded past the end of the delta list");
		return UDS_CORRUPT_DATA;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int remember_delta_index_offset(const struct delta_index_entry *delta_entry)
{
	struct delta_list *delta_list = delta_entry->delta_list;
	int result =
		ASSERT(!delta_entry->is_collision, "entry is not a collision");
	if (result != UDS_SUCCESS) {
		return result;
	}

	delta_list->save_key = delta_entry->key - delta_entry->delta;
	delta_list->save_offset = delta_entry->offset;
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_delta_index_entry(const struct delta_index *delta_index,
			  unsigned int list_number,
			  unsigned int key,
			  const byte *name,
			  bool read_only,
			  struct delta_index_entry *delta_entry)
{
	int result = start_delta_index_search(delta_index, list_number, key,
					      read_only, delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	do {
		result = next_delta_index_entry(delta_entry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	} while (!delta_entry->at_end && (key > delta_entry->key));

	result = remember_delta_index_offset(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (!delta_entry->at_end && (key == delta_entry->key)) {
		struct delta_index_entry collision_entry;
		collision_entry = *delta_entry;
		for (;;) {
			byte collision_name[COLLISION_BYTES];
			result = next_delta_index_entry(&collision_entry);
			if (result != UDS_SUCCESS) {
				return result;
			}
			if (collision_entry.at_end ||
			    !collision_entry.is_collision) {
				break;
			}
			get_bytes(delta_entry->delta_zone->memory,
				  get_collision_offset(&collision_entry),
				  collision_name,
				  COLLISION_BYTES);
			if (memcmp(collision_name, name, COLLISION_BYTES) ==
			    0) {
				*delta_entry = collision_entry;
				break;
			}
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_delta_entry_collision(const struct delta_index_entry *delta_entry,
			      byte *name)
{
	int result = assert_not_at_end(delta_entry, UDS_BAD_STATE);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_WITH_ERROR_CODE(delta_entry->is_collision,
					UDS_BAD_STATE,
					"Cannot get full block name from a"
					" non-collision delta index entry");
	if (result != UDS_SUCCESS) {
		return result;
	}

	get_bytes(delta_entry->delta_zone->memory,
		  get_collision_offset(delta_entry),
		  name,
		  COLLISION_BYTES);
	return UDS_SUCCESS;
}

/**********************************************************************/
static int assert_mutable_entry(const struct delta_index_entry *delta_entry)
{
	return ASSERT_WITH_ERROR_CODE(delta_entry->delta_list !=
					      &delta_entry->temp_delta_list,
				      UDS_BAD_STATE,
				      "delta index is mutable");
}

/**********************************************************************/
int set_delta_entry_value(const struct delta_index_entry *delta_entry,
			  unsigned int value)
{
	int result = assert_mutable_entry(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = assert_not_at_end(delta_entry, UDS_BAD_STATE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT_WITH_ERROR_CODE(((value & ((1 << delta_entry->value_bits) - 1)) == value),
					UDS_INVALID_ARGUMENT,
					"Value (%u) being set in a delta index is too large (must fit in %u bits)",
					value,
					delta_entry->value_bits);
	if (result != UDS_SUCCESS) {
		return result;
	}

	set_field(value,
		  delta_entry->delta_zone->memory,
		  get_delta_entry_offset(delta_entry),
		  delta_entry->value_bits);
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_delta_index_entry(struct delta_index_entry *delta_entry,
			  unsigned int key,
			  unsigned int value,
			  const byte *name)
{
	struct delta_memory *delta_zone;
	int result = assert_mutable_entry(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if (delta_entry->is_collision) {
		/*
		 * The caller wants us to insert a collision entry onto a
		 * collision entry.  This happens when we find a collision and
		 * attempt to add the name again to the index.  This is
		 * normally a fatal error unless we are replaying a closed
		 * chapter while we are rebuilding a volume index.
		 */
		return UDS_DUPLICATE_NAME;
	}

	if (delta_entry->offset < delta_entry->delta_list->save_offset) {
		// The saved entry offset is after the new entry and will no
		// longer be valid, so replace it with the insertion point.
		result = remember_delta_index_offset(delta_entry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	if (name != NULL) {
		// We are inserting a collision entry which is placed after
		// this entry
		result = assert_not_at_end(delta_entry, UDS_BAD_STATE);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = ASSERT((key == delta_entry->key),
				"incorrect key for collision entry");
		if (result != UDS_SUCCESS) {
			return result;
		}

		delta_entry->offset += delta_entry->entry_bits;
		set_delta(delta_entry, 0);
		set_collision(delta_entry);
		result = insert_bits(delta_entry, delta_entry->entry_bits);
	} else if (delta_entry->at_end) {
		// We are inserting a new entry at the end of the delta list
		result = ASSERT((key >= delta_entry->key),
				"key past end of list");
		if (result != UDS_SUCCESS) {
			return result;
		}

		set_delta(delta_entry, key - delta_entry->key);
		delta_entry->key = key;
		delta_entry->at_end = false;
		result = insert_bits(delta_entry, delta_entry->entry_bits);
	} else {
		int old_entry_size, additional_size;
		struct delta_index_entry next_entry;
		unsigned int next_value;
		// We are inserting a new entry which requires the delta in the
		// following entry to be updated.
		result = ASSERT((key < delta_entry->key),
				"key precedes following entry");
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = ASSERT((key >= delta_entry->key - delta_entry->delta),
				"key effects following entry's delta");
		if (result != UDS_SUCCESS) {
			return result;
		}

		old_entry_size = delta_entry->entry_bits;
		next_entry = *delta_entry;
		next_value = get_delta_entry_value(&next_entry);
		set_delta(delta_entry,
			  key - (delta_entry->key - delta_entry->delta));
		delta_entry->key = key;
		set_delta(&next_entry, next_entry.key - key);
		next_entry.offset += delta_entry->entry_bits;
		// The 2 new entries are always bigger than the 1 entry we are
		// replacing
		additional_size = delta_entry->entry_bits +
				      next_entry.entry_bits - old_entry_size;
		result = insert_bits(delta_entry, additional_size);
		if (result != UDS_SUCCESS) {
			return result;
		}
		encode_entry(&next_entry, next_value, NULL);
	}
	if (result != UDS_SUCCESS) {
		return result;
	}
	encode_entry(delta_entry, value, name);

	delta_zone = delta_entry->delta_zone;
	delta_zone->record_count++;
	delta_zone->collision_count += delta_entry->is_collision ? 1 : 0;
	return UDS_SUCCESS;
}

/**********************************************************************/
int remove_delta_index_entry(struct delta_index_entry *delta_entry)
{
	struct delta_index_entry next_entry;
	struct delta_memory *delta_zone;
	struct delta_list *delta_list;
	int result = assert_mutable_entry(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	next_entry = *delta_entry;
	result = next_delta_index_entry(&next_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	delta_zone = delta_entry->delta_zone;

	if (delta_entry->is_collision) {
		// This is a collision entry, so just remove it
		delete_bits(delta_entry, delta_entry->entry_bits);
		next_entry.offset = delta_entry->offset;
		delta_zone->collision_count -= 1;
	} else if (next_entry.at_end) {
		// This entry is at the end of the list, so just remove it
		delete_bits(delta_entry, delta_entry->entry_bits);
		next_entry.key -= delta_entry->delta;
		next_entry.offset = delta_entry->offset;
	} else {
		// The delta in the next entry needs to be updated.
		unsigned int next_value = get_delta_entry_value(&next_entry);
		int old_size = delta_entry->entry_bits + next_entry.entry_bits;
		if (next_entry.is_collision) {
			// The next record is a collision. It needs to be
			// rewritten as a non-collision with a larger delta.
			next_entry.is_collision = false;
			delta_zone->collision_count -= 1;
		}
		set_delta(&next_entry, delta_entry->delta + next_entry.delta);
		next_entry.offset = delta_entry->offset;
		// The 1 new entry is always smaller than the 2 entries we are
		// replacing
		delete_bits(delta_entry, old_size - next_entry.entry_bits);
		encode_entry(&next_entry, next_value, NULL);
	}
	delta_zone->record_count--;
	delta_zone->discard_count++;
	*delta_entry = next_entry;

	delta_list = delta_entry->delta_list;
	if (delta_entry->offset < delta_list->save_offset) {
		// The saved entry offset is after the entry we just removed
		// and it will no longer be valid.  We must force the next
		// search to start at the beginning.
		delta_list->save_key = 0;
		delta_list->save_offset = 0;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
unsigned int
get_delta_index_zone_first_list(const struct delta_index *delta_index,
				unsigned int zone_number)
{
	return delta_index->delta_zones[zone_number].first_list;
}

/**********************************************************************/
unsigned int
get_delta_index_zone_num_lists(const struct delta_index *delta_index,
			       unsigned int zone_number)
{
	return delta_index->delta_zones[zone_number].num_lists;
}

/**********************************************************************/
uint64_t
get_delta_index_zone_dlist_bits_used(const struct delta_index *delta_index,
				     unsigned int zone_number)
{
	uint64_t bit_count = 0;
	const struct delta_memory *delta_zone =
		&delta_index->delta_zones[zone_number];
	unsigned int i;
	for (i = 0; i < delta_zone->num_lists; i++) {
		bit_count +=
			get_delta_list_size(&delta_zone->delta_lists[i + 1]);
	}
	return bit_count;
}

/**********************************************************************/
uint64_t get_delta_index_dlist_bits_used(const struct delta_index *delta_index)
{
	uint64_t bit_count = 0;
	unsigned int z;
	for (z = 0; z < delta_index->num_zones; z++) {
		bit_count +=
			get_delta_index_zone_dlist_bits_used(delta_index, z);
	}
	return bit_count;
}

/**********************************************************************/
uint64_t
get_delta_index_dlist_bits_allocated(const struct delta_index *delta_index)
{
	uint64_t byte_count = 0;
	unsigned int z;
	for (z = 0; z < delta_index->num_zones; z++) {
		const struct delta_memory *delta_zone =
			&delta_index->delta_zones[z];
		byte_count += delta_zone->size;
	}
	return byte_count * CHAR_BIT;
}

/**********************************************************************/
void get_delta_index_stats(const struct delta_index *delta_index,
			   struct delta_index_stats *stats)
{
	unsigned int z;
	memset(stats, 0, sizeof(struct delta_index_stats));
	stats->memory_allocated =
		delta_index->num_zones * sizeof(struct delta_memory);
	for (z = 0; z < delta_index->num_zones; z++) {
		const struct delta_memory *delta_zone =
			&delta_index->delta_zones[z];
		stats->memory_allocated +=
			get_delta_memory_allocated(delta_zone);
		stats->rebalance_time += delta_zone->rebalance_time;
		stats->rebalance_count += delta_zone->rebalance_count;
		stats->record_count += delta_zone->record_count;
		stats->collision_count += delta_zone->collision_count;
		stats->discard_count += delta_zone->discard_count;
		stats->overflow_count += delta_zone->overflow_count;
		stats->num_lists += delta_zone->num_lists;
	}
}

/**********************************************************************/
unsigned int get_delta_index_page_count(unsigned int num_entries,
					unsigned int num_lists,
					unsigned int mean_delta,
					unsigned int num_payload_bits,
					size_t bytes_per_page)
{
	// Compute the number of bits needed for all the entries
	unsigned int bits_per_page;
	size_t bits_per_index =
		get_delta_memory_size(num_entries, mean_delta,
				      num_payload_bits);
	// Compute the number of bits needed for a single delta list
	unsigned int bits_per_delta_list = bits_per_index / num_lists;
	// Adjust the bits per index, adding the immutable delta list headers
	bits_per_index += num_lists * IMMUTABLE_HEADER_SIZE;
	// Compute the number of usable bits on an immutable index page
	bits_per_page =
		(bytes_per_page - sizeof(struct delta_page_header)) * CHAR_BIT;
	// Adjust the bits per page, taking away one immutable delta list header
	// and one delta list representing internal fragmentation
	bits_per_page -= IMMUTABLE_HEADER_SIZE + bits_per_delta_list;
	// Now compute the number of pages needed
	return (bits_per_index + bits_per_page - 1) / bits_per_page;
}

/**********************************************************************/
void log_delta_index_entry(struct delta_index_entry *delta_entry)
{
	uds_log_ratelimit(uds_log_info,
			  "List 0x%X Key 0x%X Offset 0x%X%s%s List_size 0x%X%s",
			  delta_entry->list_number,
			  delta_entry->key,
			  delta_entry->offset,
			  delta_entry->at_end ? " end" : "",
			  delta_entry->is_collision ? " collision" : "",
			  get_delta_list_size(delta_entry->delta_list),
			  delta_entry->list_overflow ? " overflow" : "");
	delta_entry->list_overflow = false;
}
