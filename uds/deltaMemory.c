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
 * $Id: //eng/uds-releases/krusty/src/uds/deltaMemory.c#13 $
 */
#include "deltaMemory.h"

#include "bits.h"
#include "buffer.h"
#include "compiler.h"
#include "errors.h"
#include "hashUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "timeUtils.h"
#include "typeDefs.h"
#include "uds.h"

/*
 * The delta_memory structure manages the memory that stores delta lists.
 *
 * The "mutable" form of delta_memory is used for the master index and for
 * an open chapter index.  The "immutable" form of delta_memory is used for
 * regular chapter indices.
 */

// This is the number of guard bits that are needed in the tail guard list
enum { GUARD_BITS = POST_FIELD_GUARD_BYTES * CHAR_BIT };

/**
 * Get the offset of the first byte that a delta list bit stream resides in
 *
 * @param delta_list  The delta list
 *
 * @return the number byte offset
 **/
static INLINE uint64_t
get_delta_list_byte_start(const struct delta_list *delta_list)
{
	return get_delta_list_start(delta_list) / CHAR_BIT;
}

/**
 * Get the actual number of bytes that a delta list bit stream resides in
 *
 * @param delta_list  The delta list
 *
 * @return the number of bytes
 **/
static INLINE uint16_t
get_delta_list_byte_size(const struct delta_list *delta_list)
{
	uint16_t start_bit_offset =
		get_delta_list_start(delta_list) % CHAR_BIT;
	uint16_t bit_size = get_delta_list_size(delta_list);
	return ((unsigned int) start_bit_offset + bit_size + CHAR_BIT - 1) /
		CHAR_BIT;
}

/**
 * Get the number of bytes in the delta lists headers.
 *
 * @param num_lists  The number of delta lists
 *
 * @return the number of bytes in the delta lists headers
 **/
static INLINE size_t get_size_of_delta_lists(unsigned int num_lists)
{
	return (num_lists + 2) * sizeof(struct delta_list);
}

/**
 * Get the size of the flags array (in bytes)
 *
 * @param num_lists  The number of delta lists
 *
 * @return the number of bytes for an array that has one bit per delta
 *         list, plus the necessary guard bytes.
 **/
static INLINE size_t get_size_of_flags(unsigned int num_lists)
{
	return (num_lists + CHAR_BIT - 1) / CHAR_BIT + POST_FIELD_GUARD_BYTES;
}

/**
 * Get the number of bytes of scratch memory for the delta lists.
 *
 * @param num_lists  The number of delta lists
 *
 * @return the number of bytes of scratch memory for the delta lists
 **/
static INLINE size_t get_size_of_temp_offsets(unsigned int num_lists)
{
	return (num_lists + 2) * sizeof(uint64_t);
}

/**********************************************************************/

/**
 * Clear the transfers flags.
 *
 * @param delta_memory  The delta memory
 **/
static void clear_transfer_flags(struct delta_memory *delta_memory)
{
	memset(delta_memory->flags,
	       0,
	       get_size_of_flags(delta_memory->num_lists));
	delta_memory->num_transfers = 0;
	delta_memory->transfer_status = UDS_SUCCESS;
}

/**********************************************************************/

/**
 * Set the transfer flags for delta lists that are not empty, and count how
 * many there are.
 *
 * @param delta_memory  The delta memory
 **/
static void flag_non_empty_delta_lists(struct delta_memory *delta_memory)
{
	clear_transfer_flags(delta_memory);
	unsigned int i;
	for (i = 0; i < delta_memory->num_lists; i++) {
		if (get_delta_list_size(&delta_memory->delta_lists[i + 1]) > 0) {
			set_one(delta_memory->flags, i, 1);
			delta_memory->num_transfers++;
		}
	}
}

/**********************************************************************/
void empty_delta_lists(struct delta_memory *delta_memory)
{
	// Zero all the delta list headers
	struct delta_list *delta_lists = delta_memory->delta_lists;
	memset(delta_lists, 0,
	       get_size_of_delta_lists(delta_memory->num_lists));

	/*
	 * Initialize delta lists to be empty. We keep 2 extra delta list
	 * descriptors, one before the first real entry and one after so that
	 * we don't need to bounds check the array access when calculating
	 * preceeding and following gap sizes.
	 *
	 * Because the delta list headers were zeroed, the head guard list is
	 * already at offset zero and size zero.
	 *
	 * The end guard list contains guard bytes so that the bit field
	 * utilities can safely read past the end of any byte we are interested
	 * in.
	 */
	uint64_t num_bits = (uint64_t) delta_memory->size * CHAR_BIT;
	delta_lists[delta_memory->num_lists + 1].start_offset =
		num_bits - GUARD_BITS;
	delta_lists[delta_memory->num_lists + 1].size = GUARD_BITS;

	// Set all the bits in the end guard list.  Do not use the bit field
	// utilities.
	memset(delta_memory->memory + delta_memory->size -
		POST_FIELD_GUARD_BYTES, ~0, POST_FIELD_GUARD_BYTES);

	// Evenly space out the real delta lists.  The sizes are already zero,
	// so we just need to set the starting offsets.
	uint64_t spacing = (num_bits - GUARD_BITS) / delta_memory->num_lists;
	uint64_t offset = spacing / 2;
	unsigned int i;
	for (i = 1; i <= delta_memory->num_lists; i++) {
		delta_lists[i].start_offset = offset;
		offset += spacing;
	}

	// Update the statistics
	delta_memory->discard_count += delta_memory->record_count;
	delta_memory->record_count = 0;
	delta_memory->collision_count = 0;
}

/**********************************************************************/
/**
 * Compute the Huffman coding parameters for the given mean delta
 *
 * @param mean_delta  The mean delta value
 * @param min_bits    The number of bits in the minimal key code
 * @param min_keys    The number of keys used in a minimal code
 * @param incr_keys   The number of keys used for another code bit
 **/
static void compute_coding_constants(unsigned int mean_delta,
				     unsigned short *min_bits,
				     unsigned int *min_keys,
				     unsigned int *incr_keys)
{
	// We want to compute the rounded value of log(2) * mean_delta.  Since
	// we cannot always use floating point, use a really good integer
	// approximation.
	*incr_keys = (836158UL * mean_delta + 603160UL) / 1206321UL;
	*min_bits = compute_bits(*incr_keys + 1);
	*min_keys = (1 << *min_bits) - *incr_keys;
}

/**********************************************************************/
/**
 * Rebalance a range of delta lists within memory.
 *
 * @param delta_memory  A delta memory structure
 * @param first         The first delta list index
 * @param last          The last delta list index
 **/
static void rebalance_delta_memory(const struct delta_memory *delta_memory,
				   unsigned int first,
				   unsigned int last)
{
	if (first == last) {
		struct delta_list *delta_list =
			&delta_memory->delta_lists[first];
		uint64_t new_start = delta_memory->temp_offsets[first];
		// We need to move only one list, and we know it is safe to do
		// so
		if (get_delta_list_start(delta_list) != new_start) {
			// Compute the first source byte
			uint64_t source =
				get_delta_list_byte_start(delta_list);
			// Update the delta list location
			delta_list->start_offset = new_start;
			// Now use the same computation to locate the first
			// destination byte
			uint64_t destination =
				get_delta_list_byte_start(delta_list);
			memmove(delta_memory->memory + destination,
				delta_memory->memory + source,
				get_delta_list_byte_size(delta_list));
		}
	} else {
		// There is more than one list. Divide the problem in half,
		// and use recursive calls to process each half.  Note that
		// after this computation, first <= middle, and middle < last.
		unsigned int middle = (first + last) / 2;
		const struct delta_list *delta_list =
			&delta_memory->delta_lists[middle];
		uint64_t new_start = delta_memory->temp_offsets[middle];
		// The direction that our middle list is moving determines
		// which half of the problem must be processed first.
		if (new_start > get_delta_list_start(delta_list)) {
			rebalance_delta_memory(delta_memory, middle + 1, last);
			rebalance_delta_memory(delta_memory, first, middle);
		} else {
			rebalance_delta_memory(delta_memory, first, middle);
			rebalance_delta_memory(delta_memory, middle + 1, last);
		}
	}
}

/**********************************************************************/
int initialize_delta_memory(struct delta_memory *delta_memory,
			    size_t size,
			    unsigned int first_list,
			    unsigned int num_lists,
			    unsigned int mean_delta,
			    unsigned int num_payload_bits)
{
	if (num_lists == 0) {
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot initialize delta memory with 0 delta lists");
	}
	byte *memory = NULL;
	int result = ALLOCATE(size, byte, "delta list", &memory);
	if (result != UDS_SUCCESS) {
		return result;
	}
	uint64_t *temp_offsets = NULL;
	result = ALLOCATE(num_lists + 2, uint64_t, "delta list temp",
			  &temp_offsets);
	if (result != UDS_SUCCESS) {
		FREE(memory);
		return result;
	}
	byte *flags = NULL;
	result = ALLOCATE(get_size_of_flags(num_lists), byte,
			  "delta list flags", &flags);
	if (result != UDS_SUCCESS) {
		FREE(memory);
		FREE(temp_offsets);
		return result;
	}

	compute_coding_constants(mean_delta,
				 &delta_memory->min_bits,
				 &delta_memory->min_keys,
				 &delta_memory->incr_keys);
	delta_memory->value_bits = num_payload_bits;
	delta_memory->memory = memory;
	delta_memory->delta_lists = NULL;
	delta_memory->temp_offsets = temp_offsets;
	delta_memory->flags = flags;
	delta_memory->buffered_writer = NULL;
	delta_memory->size = size;
	delta_memory->rebalance_time = 0;
	delta_memory->rebalance_count = 0;
	delta_memory->record_count = 0;
	delta_memory->collision_count = 0;
	delta_memory->discard_count = 0;
	delta_memory->overflow_count = 0;
	delta_memory->first_list = first_list;
	delta_memory->num_lists = num_lists;
	delta_memory->num_transfers = 0;
	delta_memory->transfer_status = UDS_SUCCESS;
	delta_memory->tag = 'm';

	// Allocate the delta lists.
	result = ALLOCATE(delta_memory->num_lists + 2, struct delta_list,
			  "delta lists", &delta_memory->delta_lists);
	if (result != UDS_SUCCESS) {
		uninitialize_delta_memory(delta_memory);
		return result;
	}

	empty_delta_lists(delta_memory);
	return UDS_SUCCESS;
}

/**********************************************************************/
void uninitialize_delta_memory(struct delta_memory *delta_memory)
{
	FREE(delta_memory->flags);
	delta_memory->flags = NULL;
	FREE(delta_memory->temp_offsets);
	delta_memory->temp_offsets = NULL;
	FREE(delta_memory->delta_lists);
	delta_memory->delta_lists = NULL;
	FREE(delta_memory->memory);
	delta_memory->memory = NULL;
}

/**********************************************************************/
void initialize_delta_memory_page(struct delta_memory *delta_memory,
				  byte *memory,
				  size_t size,
				  unsigned int num_lists,
				  unsigned int mean_delta,
				  unsigned int num_payload_bits)
{
	compute_coding_constants(mean_delta,
				 &delta_memory->min_bits,
				 &delta_memory->min_keys,
				 &delta_memory->incr_keys);
	delta_memory->value_bits = num_payload_bits;
	delta_memory->memory = memory;
	delta_memory->delta_lists = NULL;
	delta_memory->temp_offsets = NULL;
	delta_memory->flags = NULL;
	delta_memory->buffered_writer = NULL;
	delta_memory->size = size;
	delta_memory->rebalance_time = 0;
	delta_memory->rebalance_count = 0;
	delta_memory->record_count = 0;
	delta_memory->collision_count = 0;
	delta_memory->discard_count = 0;
	delta_memory->overflow_count = 0;
	delta_memory->first_list = 0;
	delta_memory->num_lists = num_lists;
	delta_memory->num_transfers = 0;
	delta_memory->transfer_status = UDS_SUCCESS;
	delta_memory->tag = 'p';
}

/**********************************************************************/
bool are_delta_memory_transfers_done(const struct delta_memory *delta_memory)
{
	return delta_memory->num_transfers == 0;
}

/**********************************************************************/
int start_restoring_delta_memory(struct delta_memory *delta_memory)
{
	// Extend and balance memory to receive the delta lists
	int result = extend_delta_memory(delta_memory, 0, 0, false);
	if (result != UDS_SUCCESS) {
		return UDS_SUCCESS;
	}

	// The tail guard list needs to be set to ones
	struct delta_list *delta_list =
		&delta_memory->delta_lists[delta_memory->num_lists + 1];
	set_one(delta_memory->memory,
		get_delta_list_start(delta_list),
		get_delta_list_size(delta_list));

	flag_non_empty_delta_lists(delta_memory);
	return UDS_SUCCESS;
}

/**********************************************************************/
static int __must_check
read_delta_list_save_info(struct buffered_reader *reader,
			  struct delta_list_save_info *dlsi)
{
	byte buffer[sizeof(struct delta_list_save_info)];
	int result = read_from_buffered_reader(reader, buffer, sizeof(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}
	dlsi->tag = buffer[0];
	dlsi->bit_offset = buffer[1];
	dlsi->byte_count = get_unaligned_le16(&buffer[2]);
	dlsi->index = get_unaligned_le32(&buffer[4]);
	return result;
}

/**********************************************************************/
int read_saved_delta_list(struct delta_list_save_info *dlsi,
			  byte data[DELTA_LIST_MAX_BYTE_COUNT],
			  struct buffered_reader *buffered_reader)
{
	int result = read_delta_list_save_info(buffered_reader, dlsi);
	if (result == UDS_END_OF_FILE) {
		return UDS_END_OF_FILE;
	}
	if (result != UDS_SUCCESS) {
		return logWarningWithStringError(result,
						 "failed to read delta list data");
	}
	if ((dlsi->bit_offset >= CHAR_BIT) ||
	    (dlsi->byte_count > DELTA_LIST_MAX_BYTE_COUNT)) {
		return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
						 "corrupt delta list data");
	}
	if (dlsi->tag == 'z') {
		return UDS_END_OF_FILE;
	}
	result = read_from_buffered_reader(buffered_reader, data,
					   dlsi->byte_count);
	if (result != UDS_SUCCESS) {
		return logWarningWithStringError(result,
						 "failed to read delta list data");
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int restore_delta_list(struct delta_memory *delta_memory,
		       const struct delta_list_save_info *dlsi,
		       const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
	unsigned int list_number = dlsi->index - delta_memory->first_list;
	if (list_number >= delta_memory->num_lists) {
		return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
						 "invalid delta list number %u not in range [%u,%u)",
						 dlsi->index,
						 delta_memory->first_list,
						 delta_memory->first_list +
						 	delta_memory->num_lists);
	}

	if (get_field(delta_memory->flags, list_number, 1) == 0) {
		return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
						 "unexpected delta list number %u",
						 dlsi->index);
	}

	struct delta_list *delta_list =
		&delta_memory->delta_lists[list_number + 1];
	uint16_t bit_size = get_delta_list_size(delta_list);
	unsigned int byte_count =
		((unsigned int) dlsi->bit_offset + bit_size + CHAR_BIT - 1) /
		CHAR_BIT;
	if (dlsi->byte_count != byte_count) {
		return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
						 "unexpected delta list size %u != %u",
						 dlsi->byte_count,
						 byte_count);
	}

	move_bits(data,
		  dlsi->bit_offset,
		  delta_memory->memory,
		  get_delta_list_start(delta_list),
		  bit_size);
	set_zero(delta_memory->flags, list_number, 1);
	delta_memory->num_transfers--;
	return UDS_SUCCESS;
}

/**********************************************************************/
void abort_restoring_delta_memory(struct delta_memory *delta_memory)
{
	clear_transfer_flags(delta_memory);
	empty_delta_lists(delta_memory);
}

/**********************************************************************/
void start_saving_delta_memory(struct delta_memory *delta_memory,
			       struct buffered_writer *buffered_writer)
{
	flag_non_empty_delta_lists(delta_memory);
	delta_memory->buffered_writer = buffered_writer;
}

/**********************************************************************/
int finish_saving_delta_memory(struct delta_memory *delta_memory)
{
	unsigned int i;
	for (i = 0;
	     !are_delta_memory_transfers_done(delta_memory) &&
		(i < delta_memory->num_lists);

	     i++) {
		lazy_flush_delta_list(delta_memory, i);
	}
	if (delta_memory->num_transfers > 0) {
		delta_memory->transfer_status =
			logWarningWithStringError(UDS_CORRUPT_DATA,
						  "Not all delta lists written");
	}
	delta_memory->buffered_writer = NULL;
	return delta_memory->transfer_status;
}

/**********************************************************************/
void abort_saving_delta_memory(struct delta_memory *delta_memory)
{
	clear_transfer_flags(delta_memory);
	delta_memory->buffered_writer = NULL;
}

/**********************************************************************/
static int __must_check
write_delta_list_save_info(struct buffered_writer *buffered_writer,
			   struct delta_list_save_info *dlsi)
{
	byte buffer[sizeof(struct delta_list_save_info)];
	buffer[0] = dlsi->tag;
	buffer[1] = dlsi->bit_offset;
	put_unaligned_le16(dlsi->byte_count, &buffer[2]);
	put_unaligned_le32(dlsi->index, &buffer[4]);
	return write_to_buffered_writer(buffered_writer, buffer,
					sizeof(buffer));
}

/**********************************************************************/
void flush_delta_list(struct delta_memory *delta_memory,
		      unsigned int flush_index)
{
	ASSERT_LOG_ONLY((get_field(delta_memory->flags, flush_index, 1) != 0),
			"flush bit is set");
	set_zero(delta_memory->flags, flush_index, 1);
	delta_memory->num_transfers--;

	struct delta_list *delta_list =
		&delta_memory->delta_lists[flush_index + 1];
	struct delta_list_save_info dlsi;
	dlsi.tag = delta_memory->tag;
	dlsi.bit_offset = get_delta_list_start(delta_list) % CHAR_BIT;
	dlsi.byte_count = get_delta_list_byte_size(delta_list);
	dlsi.index = delta_memory->first_list + flush_index;

	int result = write_delta_list_save_info(delta_memory->buffered_writer,
						&dlsi);
	if (result != UDS_SUCCESS) {
		if (delta_memory->transfer_status == UDS_SUCCESS) {
			logWarningWithStringError(result,
						  "failed to write delta list memory");
			delta_memory->transfer_status = result;
		}
	}
	result = write_to_buffered_writer(delta_memory->buffered_writer,
		delta_memory->memory + get_delta_list_byte_start(delta_list),
		dlsi.byte_count);
	if (result != UDS_SUCCESS) {
		if (delta_memory->transfer_status == UDS_SUCCESS) {
			logWarningWithStringError(result,
						  "failed to write delta list memory");
			delta_memory->transfer_status = result;
		}
	}
}

/**********************************************************************/
int write_guard_delta_list(struct buffered_writer *buffered_writer)
{
	struct delta_list_save_info dlsi;
	dlsi.tag = 'z';
	dlsi.bit_offset = 0;
	dlsi.byte_count = 0;
	dlsi.index = 0;
	int result =
		write_to_buffered_writer(buffered_writer,
					 (const byte *) &dlsi,
					 sizeof(struct delta_list_save_info));
	if (result != UDS_SUCCESS) {
		logWarningWithStringError(result,
					  "failed to write guard delta list");
	}
	return result;
}

/**********************************************************************/
int extend_delta_memory(struct delta_memory *delta_memory,
			unsigned int growing_index,
			size_t growing_size,
			bool do_copy)
{
	if (!is_mutable(delta_memory)) {
		return logErrorWithStringError(UDS_BAD_STATE,
					       "Attempt to read into an immutable delta list memory");
	}

	ktime_t start_time = currentTime(CLOCK_MONOTONIC);

	// Calculate the amount of space that is in use.  Include the space
	// that has a planned use.
	struct delta_list *delta_lists = delta_memory->delta_lists;
	size_t used_space = growing_size;
	unsigned int i;
	for (i = 0; i <= delta_memory->num_lists + 1; i++) {
		used_space += get_delta_list_byte_size(&delta_lists[i]);
	}

	if (delta_memory->size < used_space) {
		return UDS_OVERFLOW;
	}

	// Compute the new offsets of the delta lists
	size_t spacing =
		(delta_memory->size - used_space) / delta_memory->num_lists;
	delta_memory->temp_offsets[0] = 0;
	for (i = 0; i <= delta_memory->num_lists; i++) {
		delta_memory->temp_offsets[i + 1] =
			(delta_memory->temp_offsets[i] +
			 get_delta_list_byte_size(&delta_lists[i]) + spacing);
		delta_memory->temp_offsets[i] *= CHAR_BIT;
		delta_memory->temp_offsets[i] +=
			get_delta_list_start(&delta_lists[i]) % CHAR_BIT;
		if (i == 0) {
			delta_memory->temp_offsets[i + 1] -= spacing / 2;
		}
		if (i + 1 == growing_index) {
			delta_memory->temp_offsets[i + 1] += growing_size;
		}
	}
	delta_memory->temp_offsets[delta_memory->num_lists + 1] =
		(delta_memory->size * CHAR_BIT -
		 get_delta_list_size(&delta_lists[delta_memory->num_lists + 1]));
	// When we rebalance the delta list, we will include the end guard list
	// in the rebalancing.  It contains the end guard data, which must be
	// copied.
	if (do_copy) {
		rebalance_delta_memory(delta_memory, 1,
				       delta_memory->num_lists + 1);
		ktime_t end_time = currentTime(CLOCK_MONOTONIC);
		delta_memory->rebalance_count++;
		delta_memory->rebalance_time +=
			timeDifference(end_time, start_time);
	} else {
		for (i = 1; i <= delta_memory->num_lists + 1; i++) {
			delta_lists[i].start_offset =
				delta_memory->temp_offsets[i];
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int validate_delta_lists(const struct delta_memory *delta_memory)
{
	// Validate the delta index fields set by restoring a delta index
	if (delta_memory->collision_count > delta_memory->record_count) {
		return logWarningWithStringError(UDS_BAD_STATE,
						 "delta index contains more collisions (%ld) than records (%ld)",
						 delta_memory->collision_count,
						 delta_memory->record_count);
	}

	// Validate the delta lists
	struct delta_list *delta_lists = delta_memory->delta_lists;
	if (get_delta_list_start(&delta_lists[0]) != 0) {
		return logWarningWithStringError(UDS_BAD_STATE,
						 "the head guard delta list does not start at 0: %llu",
						 get_delta_list_start(&delta_lists[0]));
	}
	uint64_t num_bits =
		get_delta_list_end(&delta_lists[delta_memory->num_lists + 1]);
	if (num_bits != delta_memory->size * CHAR_BIT) {
		return logWarningWithStringError(UDS_BAD_STATE,
						 "the tail guard delta list does not end at end of allocated memory:  %llu != %zd",
						 num_bits,
						 delta_memory->size * CHAR_BIT);
	}
	int num_guard_bits =
		get_delta_list_size(&delta_lists[delta_memory->num_lists + 1]);
	if (num_guard_bits < GUARD_BITS) {
		return logWarningWithStringError(UDS_BAD_STATE,
						 "the tail guard delta list does not contain sufficient guard bits:  %d < %d",
						 num_guard_bits,
						 GUARD_BITS);
	}
	unsigned int i;
	for (i = 0; i <= delta_memory->num_lists + 1; i++) {
		if (get_delta_list_start(&delta_lists[i]) >
		    get_delta_list_end(&delta_lists[i])) {
			return logWarningWithStringError(UDS_BAD_STATE,
							 "invalid delta list %u: [%llu, %llu)",
							 i,
							 get_delta_list_start(&delta_lists[i]),
							 get_delta_list_end(&delta_lists[i]));
		}
		if (i > delta_memory->num_lists) {
			// The rest of the checks do not apply to the tail guard
			// list
			continue;
		}
		if (get_delta_list_end(&delta_lists[i]) >
		    get_delta_list_start(&delta_lists[i + 1])) {
			return logWarningWithStringError(UDS_BAD_STATE,
							 "delta lists %u and %u overlap:  %llu > %llu",
							 i, i + 1,
							 get_delta_list_end(&delta_lists[i]),
							 get_delta_list_start(&delta_lists[i + 1]));
		}
		if (i == 0) {
			// The rest of the checks do not apply to the head guard
			// list
			continue;
		}
		if (delta_lists[i].save_offset >
		    get_delta_list_size(&delta_lists[i])) {
			return logWarningWithStringError(UDS_BAD_STATE,
							 "delta lists %u saved offset is larger than the list:  %u > %u",
							 i,
							 delta_lists[i].save_offset,
							 get_delta_list_size(&delta_lists[i]));
		}
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
size_t get_delta_memory_allocated(const struct delta_memory *delta_memory)
{
	return (delta_memory->size +
		get_size_of_delta_lists(delta_memory->num_lists) +
		get_size_of_flags(delta_memory->num_lists) +
		get_size_of_temp_offsets(delta_memory->num_lists));
}

/**********************************************************************/
size_t get_delta_memory_size(unsigned long num_entries,
			     unsigned int mean_delta,
			     unsigned int num_payload_bits)
{
	unsigned short min_bits;
	unsigned int incr_keys, min_keys;
	compute_coding_constants(mean_delta, &min_bits, &min_keys, &incr_keys);
	// On average, each delta is encoded into about min_bits+1.5 bits.
	return (num_entries * (num_payload_bits + min_bits + 1) +
		num_entries / 2);
}
