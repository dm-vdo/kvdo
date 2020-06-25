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
 * $Id: //eng/uds-releases/krusty/src/uds/deltaMemory.h#8 $
 */

#ifndef DELTAMEMORY_H
#define DELTAMEMORY_H 1

#include "bits.h"
#include "bufferedReader.h"
#include "bufferedWriter.h"
#include "compiler.h"
#include "cpu.h"
#include "timeUtils.h"

/*
 * We encode the delta list information into 16 bytes per list.
 *
 * Because the master index has 1 million delta lists, each byte of header
 * information ends up costing us 1MB.  We have an incentive to keep the
 * size down.
 *
 * The master index delta list memory is currently about 780MB in size,
 * which is more than 6 gigabits.  Therefore we need at least 33 bits to
 * address the master index memory and we use the uint64_t type.
 *
 * The master index delta lists have 256 entries of about 24 bits each,
 * which is 6K bits.  The index needs 13 bits to represent the size of a
 * delta list and we use the uint16_t type.
 */

struct delta_list {
	uint64_t start_offset;  // The offset of the delta list start within
				// memory
	uint16_t size;          // The number of bits in the delta list
	uint16_t save_offset;   // Where the last search "found" the key
	unsigned int save_key;  // The key for the record just before
				// save_offset.
};

struct delta_memory {
	byte *memory;                             // The delta list memory
	struct delta_list *delta_lists;           // The delta list headers
	uint64_t *temp_offsets;                   // Temporary starts of delta
						  // lists
	byte *flags;                              // Transfer flags
	struct buffered_writer *buffered_writer;  // Buffered writer for saving
						  // an index
	size_t size;                              // The size of delta list
						  // memory
	rel_time_t rebalance_time;                // The time spent rebalancing
	int rebalance_count;                      // Number of memory
						  // rebalances
	unsigned short value_bits;                // The number of bits of
						  // value
	unsigned short min_bits;                  // The number of bits in the
						  // minimal key code
	unsigned int min_keys;                    // The number of keys used in
						  // a minimal code
	unsigned int incr_keys;                   // The number of keys used
						  // for another code bit
	long record_count;                        // The number of records in
						  // the index
	long collision_count;                     // The number of collision
						  // records
	long discard_count;                       // The number of records
						  // removed
	long overflow_count;                      // The number of
						  // UDS_OVERFLOWs detected
	unsigned int first_list;                  // The index of the first
						  // delta list
	unsigned int num_lists;                   // The number of delta lists
	unsigned int num_transfers;               // Number of transfer flags
						  // that are set
	int transfer_status;                      // Status of the transfers in
						  // progress
	byte tag;                                 // Tag belonging to this
						  // delta index
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct delta_list_save_info {
	uint8_t  tag;         // Tag identifying which delta index this list
			      // is in
	uint8_t  bit_offset;  // Bit offset of the start of the list data
	uint16_t byte_count;  // Number of bytes of list data
	uint32_t index;       // The delta list number within the delta index
};

// The maximum size of a single delta list (in bytes).  We add guard bytes
// to this because such a buffer can be used with move_bits.
enum {
	DELTA_LIST_MAX_BYTE_COUNT =
		((UINT16_MAX + CHAR_BIT) / CHAR_BIT + POST_FIELD_GUARD_BYTES)
};

/**
 * Initialize delta list memory.
 *
 * @param delta_memory      A delta memory structure
 * @param size              The initial size of the memory array
 * @param first_list        The index of the first delta list
 * @param num_lists         The number of delta lists
 * @param mean_delta        The mean delta
 * @param num_payload_bits  The number of payload bits
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check initialize_delta_memory(struct delta_memory *delta_memory,
					 size_t size,
					 unsigned int first_list,
					 unsigned int num_lists,
					 unsigned int mean_delta,
					 unsigned int num_payload_bits);

/**
 * Uninitialize delta list memory.
 *
 * @param delta_memory  A delta memory structure
 **/
void uninitialize_delta_memory(struct delta_memory *delta_memory);

/**
 * Initialize delta list memory to refer to a cached page.
 *
 * @param delta_memory      A delta memory structure
 * @param memory            The memory page
 * @param size              The size of the memory page
 * @param num_lists         The number of delta lists
 * @param mean_delta        The mean delta
 * @param num_payload_bits  The number of payload bits
 **/
void initialize_delta_memory_page(struct delta_memory *delta_memory,
				  byte *memory,
				  size_t size,
				  unsigned int num_lists,
				  unsigned int mean_delta,
				  unsigned int num_payload_bits);

/**
 * Empty the delta lists.
 *
 * @param delta_memory  The delta memory
 **/
void empty_delta_lists(struct delta_memory *delta_memory);

/**
 * Is there a delta list memory save or restore in progress?
 *
 * @param delta_memory  A delta memory structure
 *
 * @return true if there are no delta lists that need to be saved or
 *         restored
 **/
bool are_delta_memory_transfers_done(const struct delta_memory *delta_memory);

/**
 * Start restoring delta list memory from a file descriptor
 *
 * @param delta_memory  A delta memory structure
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check
start_restoring_delta_memory(struct delta_memory *delta_memory);

/**
 * Read a saved delta list from a file descriptor
 *
 * @param dlsi             The delta_list_save_info describing the delta list
 * @param data             The saved delta list bit stream
 * @param buffered_reader  The buffered reader to read the delta list from
 *
 * @return error code or UDS_SUCCESS
 *         or UDS_END_OF_FILE at end of the data stream
 **/
int __must_check read_saved_delta_list(struct delta_list_save_info *dlsi,
				       byte data[DELTA_LIST_MAX_BYTE_COUNT],
				       struct buffered_reader *buffered_reader);

/**
 * Restore a saved delta list
 *
 * @param delta_memory  A delta memory structure
 * @param dlsi          The delta_list_save_info describing the delta list
 * @param data          The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check restore_delta_list(struct delta_memory *delta_memory,
				    const struct delta_list_save_info *dlsi,
				    const byte data[DELTA_LIST_MAX_BYTE_COUNT]);

/**
 * Abort restoring delta list memory from an input stream.
 *
 * @param delta_memory  A delta memory structure
 **/
void abort_restoring_delta_memory(struct delta_memory *delta_memory);

/**
 * Start saving delta list memory to a buffered output stream
 *
 * @param delta_memory     A delta memory structure
 * @param buffered_writer  The index state component being written
 **/
void start_saving_delta_memory(struct delta_memory *delta_memory,
			       struct buffered_writer *buffered_writer);

/**
 * Finish saving delta list memory to an output stream.  Force the writing
 * of all of the remaining data.  If an error occurred asynchronously
 * during the save operation, it will be returned here.
 *
 * @param delta_memory  A delta memory structure
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check finish_saving_delta_memory(struct delta_memory *delta_memory);

/**
 * Abort saving delta list memory to an output stream.  If an error
 * occurred asynchronously during the save operation, it will be dropped.
 *
 * @param delta_memory  A delta memory structure
 **/
void abort_saving_delta_memory(struct delta_memory *delta_memory);

/**
 * Flush a delta list to an output stream
 *
 * @param delta_memory  A delta memory structure
 * @param flush_index   Index of the delta list that may need to be flushed.
 **/
void flush_delta_list(struct delta_memory *delta_memory,
		      unsigned int flush_index);

/**
 * Write a guard delta list to mark the end of the saved data
 *
 * @param buffered_writer  The buffered writer to write the guard delta list to
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check
write_guard_delta_list(struct buffered_writer *buffered_writer);

/**
 * Extend the memory used by the delta lists and rebalance the lists in the
 * new chunk.
 *
 * <p> The delta memory contains N delta lists, which are guarded by two
 * empty delta lists.  The valid delta lists are numbered 1 to N, and the
 * guards are numbered 0 and (N+1);
 *
 * <p> When the delta lista are bit streams, it is possible that the tail
 * of list J and the head of list (J+1) are in the same byte.  In this case
 * old_offsets[j]+sizes[j]==old_offset[j]-1.  We handle this correctly.
 *
 * @param delta_memory   A delta memory structure
 * @param growing_index  Index of the delta list that needs additional space
 *                       left before it (from 1 to N+1).
 * @param growing_size   Number of additional bytes needed before growing_index
 * @param do_copy        True to copy the data, False to just balance the space
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check extend_delta_memory(struct delta_memory *delta_memory,
				     unsigned int growing_index,
				     size_t growing_size,
				     bool do_copy);

/**
 * Validate the delta list headers.
 *
 * @param delta_memory   A delta memory structure
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check validate_delta_lists(const struct delta_memory *delta_memory);

/**
 * Get the number of bytes allocated for delta index entries and any
 * associated overhead.
 *
 * @param delta_memory   A delta memory structure
 *
 * @return The number of bytes allocated
 **/
size_t get_delta_memory_allocated(const struct delta_memory *delta_memory);

/**
 * Get the expected number of bits used in a delta index
 *
 * @param num_entries       The number of index entries
 * @param mean_delta        The mean delta value
 * @param num_payload_bits  The number of bits in the payload or value
 *
 * @return  The expected size of a delta index in bits
 **/
size_t __must_check get_delta_memory_size(unsigned long num_entries,
					  unsigned int mean_delta,
					  unsigned int num_payload_bits);

/**
 * Get the bit offset to the start of the delta list bit stream
 *
 * @param delta_list  The delta list header
 *
 * @return the start of the delta list
 **/
static INLINE uint64_t
get_delta_list_start(const struct delta_list *delta_list)
{
	return delta_list->start_offset;
}

/**
 * Get the number of bits in a delta list bit stream
 *
 * @param delta_list  The delta list header
 *
 * @return the size of the delta list
 **/
static INLINE uint16_t
get_delta_list_size(const struct delta_list *delta_list)
{
	return delta_list->size;
}

/**
 * Get the bit offset to the end of the delta list bit stream
 *
 * @param delta_list  The delta list header
 *
 * @return the end of the delta list
 **/
static INLINE uint64_t get_delta_list_end(const struct delta_list *delta_list)
{
	return get_delta_list_start(delta_list) +
		get_delta_list_size(delta_list);
}

/**
 * Identify mutable vs. immutable delta memory
 *
 * Mutable delta memory contains delta lists that can be modified, and is
 * initialized using initialize_delta_memory().
 *
 * Immutable delta memory contains packed delta lists, cannot be modified,
 * and is initialized using initialize_delta_memory_page().
 *
 * For mutable delta memory, all of the following expressions are true.
 * And for immutable delta memory, all of the following expressions are
 * false.
 *             delta_lists != NULL
 *             temp_offsets != NULL
 *             flags != NULL
 *
 * @param delta_memory  A delta memory structure
 *
 * @return true if the delta memory is mutable
 **/
static INLINE bool is_mutable(const struct delta_memory *delta_memory)
{
	return delta_memory->delta_lists != NULL;
}

/**
 * Lazily flush a delta list to an output stream
 *
 * @param delta_memory  A delta memory structure
 * @param flush_index   Index of the delta list that may need to be flushed.
 **/
static INLINE void lazy_flush_delta_list(struct delta_memory *delta_memory,
					 unsigned int flush_index)
{
	if (get_field(delta_memory->flags, flush_index, 1) != 0) {
		flush_delta_list(delta_memory, flush_index);
	}
}
#endif /* DELTAMEMORY_H */
