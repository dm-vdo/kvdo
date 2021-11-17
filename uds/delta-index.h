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
#include "delta-memory.h"

enum {
	/* the number of extra bytes and bits needed to store a collision entry */
	COLLISION_BYTES = UDS_CHUNK_NAME_SIZE,
	COLLISION_BITS = COLLISION_BYTES * CHAR_BIT
};

struct delta_index {
	struct delta_memory *delta_zones;  /* The zones */
	unsigned int num_zones;            /* The number of zones */
	unsigned int num_lists;            /* The number of delta lists */
	unsigned int lists_per_zone;       /* Lists per zone (last zone can be */
				           /* smaller) */
	bool is_mutable;                   /* True if this index is mutable */
	byte tag;                          /* Tag belonging to this delta index */
};

/*
 * A delta_index_page describes a single page of a chapter index.  The
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
 * delta_index.  The fields documented as "private" carry information
 * between delta_index method calls and should not be used outside the
 * delta_index module.
 *
 * (1) The delta_index_entry is used like an iterator when searching a delta
 *     list.
 *
 * (2) And it is also the result of a successful search and can be used to
 *     refer to the element found by the search.
 *
 * (3) And it is also the result of an unsuccessful search and can be used
 *     to refer to the insertion point for a new record.
 *
 * (4) If at_end==true, the delta_list entry can only be used as the insertion
 *     point for a new record at the end of the list.
 *
 * (5) If at_end==false and is_collision==true, the delta_list entry fields
 *     refer to a collision entry in the list, and the delta_list entry can
 *     be used a a reference to this entry.
 *
 * (6) If at_end==false and is_collision==false, the delta_list entry fields
 *     refer to a non-collision entry in the list.  Such delta_list entries
 *     can be used as a reference to a found entry, or an insertion point
 *     for a non-collision entry before this entry, or an insertion point
 *     for a collision entry that collides with this entry.
 */

struct delta_index_entry {
	/* Public fields */
	unsigned int key;                   /* The key for this entry */
	bool at_end;                        /* We are after the last entry in */
					    /* the list */
	bool is_collision;                  /* This record is a collision */
	/* Private fields (but DeltaIndex_t1 cheats and looks at them) */
	bool list_overflow;                 /* This delta list overflowed */
	unsigned short value_bits;          /* The number of bits used for */
					    /* the value */
	unsigned short entry_bits;          /* The number of bits used for */
					    /* the entire entry */
	struct delta_memory *delta_zone;    /* The delta index zone */
	struct delta_list *delta_list;      /* The delta list containing */
					    /* the entry */
	unsigned int list_number;           /* The delta list number */
	uint32_t offset;                    /* Bit offset of this entry within */
					    /* the list */
	unsigned int delta;                 /* The delta between this and */
					    /* previous entry */
	struct delta_list temp_delta_list;  /* Temporary delta list for */
					    /* immutable indices */
};

struct delta_index_stats {
	size_t memory_allocated;    /* Number of bytes allocated */
	ktime_t rebalance_time;	    /* Nanoseconds spent rebalancing */
	int rebalance_count;        /* Number of memory rebalances */
	long record_count;          /* The number of records in the index */
	long collision_count;       /* The number of collision records */
	long discard_count;         /* The number of records removed */
	long overflow_count;        /* The number of UDS_OVERFLOWs detected */
	unsigned int num_lists;     /* The number of delta lists */
};

/**
 * Initialize a delta index.
 *
 * @param delta_index       The delta index to initialize
 * @param num_zones         The number of zones in the index
 * @param num_lists         The number of delta lists in the index
 * @param mean_delta        The mean delta value
 * @param num_payload_bits  The number of bits in the payload or value
 * @param memory_size       The number of bytes in memory for the index
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check initialize_delta_index(struct delta_index *delta_index,
					unsigned int num_zones,
					unsigned int num_lists,
					unsigned int mean_delta,
					unsigned int num_payload_bits,
					size_t memory_size);

/**
 * Initialize an immutable delta index page.
 *
 * @param delta_index_page  The delta index page to initialize
 * @param expected_nonce    If non-zero, the expected nonce.
 * @param mean_delta        The mean delta value
 * @param num_payload_bits  The number of bits in the payload or value
 * @param memory            The memory page
 * @param mem_size          The size of the memory page
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check
initialize_delta_index_page(struct delta_index_page *delta_index_page,
			    uint64_t expected_nonce,
			    unsigned int mean_delta,
			    unsigned int num_payload_bits,
			    byte *memory,
			    size_t mem_size);

/**
 * Uninitialize a delta index.
 *
 * @param delta_index  The delta index to uninitialize
 **/
void uninitialize_delta_index(struct delta_index *delta_index);

/**
 * Empty the delta index.
 *
 * @param delta_index  The delta index being emptied.
 **/
void empty_delta_index(const struct delta_index *delta_index);

/**
 * Empty a zone of the delta index.
 *
 * @param delta_index  The delta index
 * @param zone_number  The zone being emptied
 **/
void empty_delta_index_zone(const struct delta_index *delta_index,
			    unsigned int zone_number);

/**
 * Pack delta lists from a mutable delta index into an immutable delta index
 * page.  A range of delta lists (starting with a specified list index) is
 * copied from the mutable delta index into a memory page used in the immutable
 * index.  The number of lists copied onto the page is returned to the caller.
 *
 * @param delta_index             The delta index being converted
 * @param header_nonce            The header nonce to store
 * @param memory                  The memory page to use
 * @param mem_size                The size of the memory page
 * @param virtual_chapter_number  The virtual chapter number
 * @param first_list              The first delta list number to be copied
 * @param num_lists               The number of delta lists that were copied
 *
 * @return error code or UDS_SUCCESS.  On UDS_SUCCESS, the numLists
 *         argument contains the number of lists copied.
 **/
int __must_check pack_delta_index_page(const struct delta_index *delta_index,
				       uint64_t header_nonce,
				       byte *memory,
				       size_t mem_size,
				       uint64_t virtual_chapter_number,
				       unsigned int first_list,
				       unsigned int *num_lists);


/**
 * Set the tag value used when saving and/or restoring a delta index.
 *
 * @param delta_index  The delta index
 * @param tag          The tag value
 **/
void set_delta_index_tag(struct delta_index *delta_index, byte tag);

/**
 * Start restoring a delta index from an input stream.
 *
 * @param delta_index       The delta index to read into
 * @param buffered_readers  The buffered readers to read the delta index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check
start_restoring_delta_index(const struct delta_index *delta_index,
			    struct buffered_reader **buffered_readers,
			    int num_readers);

/**
 * Have all the data been read while restoring a delta index from an
 * input stream?
 *
 * @param delta_index  The delta index
 *
 * @return true if all the data are read
 **/
bool is_restoring_delta_index_done(const struct delta_index *delta_index);

/**
 * Restore a saved delta list
 *
 * @param delta_index  The delta index
 * @param dlsi         The delta_list_save_info describing the delta list
 * @param data         The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check
restore_delta_list_to_delta_index(const struct delta_index *delta_index,
				  const struct delta_list_save_info *dlsi,
				  const byte data[DELTA_LIST_MAX_BYTE_COUNT]);

/**
 * Abort restoring a delta index from an input stream.
 *
 * @param delta_index  The delta index
 **/
void abort_restoring_delta_index(const struct delta_index *delta_index);

/**
 * Start saving a delta index zone to a buffered output stream.
 *
 * @param delta_index      The delta index
 * @param zone_number      The zone number
 * @param buffered_writer  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check
start_saving_delta_index(const struct delta_index *delta_index,
			 unsigned int zone_number,
			 struct buffered_writer *buffered_writer);

/**
 * Have all the data been written while saving a delta index zone to an
 * output stream?  If the answer is yes, it is still necessary to call
 * finish_saving_delta_index(), which will return quickly.
 *
 * @param delta_index  The delta index
 * @param zone_number  The zone number
 *
 * @return true if all the data are written
 **/
bool is_saving_delta_index_done(const struct delta_index *delta_index,
				unsigned int zone_number);

/**
 * Finish saving a delta index zone to an output stream.  Force the writing
 * of all of the remaining data.  If an error occurred asynchronously
 * during the save operation, it will be returned here.
 *
 * @param delta_index  The delta index
 * @param zone_number  The zone number
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check finish_saving_delta_index(const struct delta_index *delta_index,
					   unsigned int zone_number);

/**
 * Abort saving a delta index zone to an output stream.  If an error
 * occurred asynchronously during the save operation, it will be dropped.
 *
 * @param delta_index  The delta index
 * @param zone_number  The zone number
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check abort_saving_delta_index(const struct delta_index *delta_index,
					  unsigned int zone_number);

/**
 * Compute the number of bytes required to save a delta index
 *
 * @param num_lists    The number of delta lists in the index
 * @param memory_size  The number of bytes in memory for the index
 *
 * @return num_bytes  The number of bytes required to save the volume index
 **/
size_t __must_check compute_delta_index_save_bytes(unsigned int num_lists,
						   size_t memory_size);

/**
 * Validate the delta index
 *
 * @param delta_index  The delta index
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check validate_delta_index(const struct delta_index *delta_index);

/**
 * Prepare to search for an entry in the specified delta list.
 *
 * <p> This is always the first routine to be called when dealing with delta
 * index entries. It is always followed by calls to next_delta_index_entry to
 * iterate through a delta list. The fields of the delta_index_entry argument
 * will be set up for iteration, but will not contain an entry from the list.
 *
 * @param delta_index  The delta index to search
 * @param list_number  The delta list number
 * @param key          First delta list key that the caller is interested in
 * @param read_only    True if this is a read-only operation
 * @param iterator     The index entry being used to search through the list
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check start_delta_index_search(const struct delta_index *delta_index,
					  unsigned int list_number,
					  unsigned int key,
					  bool read_only,
					  struct delta_index_entry *iterator);

/**
 * Find the next entry in the specified delta list
 *
 * @param delta_entry  Info about an entry, which is updated to describe the
 *                     following entry
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check next_delta_index_entry(struct delta_index_entry *delta_entry);

/**
 * Remember the position of a delta index entry, so that we can use it when
 * starting the next search.
 *
 * @param delta_entry  Info about an entry found during a search.  This should
 *                     be the first entry that matches the key exactly (i.e.
 *                     not a collision entry), or the first entry with a key
 *                     greater than the entry sought for.
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check
remember_delta_index_offset(const struct delta_index_entry *delta_entry);

/**
 * Find the delta index entry, or the insertion point for a delta index
 * entry.
 *
 * @param delta_index  The delta index to search
 * @param list_number  The delta list number
 * @param key          The key field being looked for
 * @param name         The 256 bit full name
 * @param read_only    True if this is a read-only index search
 * @param delta_entry  Updated to describe the entry being looked for
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_delta_index_entry(const struct delta_index *delta_index,
				       unsigned int list_number,
				       unsigned int key,
				       const byte *name,
				       bool read_only,
				       struct delta_index_entry *delta_entry);

/**
 * Get the full name from a collision delta_index_entry
 *
 * @param delta_entry  The delta index record
 * @param name         The 256 bit full name
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
get_delta_entry_collision(const struct delta_index_entry *delta_entry,
			  byte *name);

/**
 * Get the bit offset into delta memory of a delta index entry.
 *
 * @param delta_entry  The delta index entry
 *
 * @return the bit offset into delta memory
 **/
static INLINE uint64_t
get_delta_entry_offset(const struct delta_index_entry *delta_entry)
{
	return get_delta_list_start(delta_entry->delta_list) +
		delta_entry->offset;
}

/**
 * Get the number of bits used to encode the entry key (the delta).
 *
 * @param entry         The delta index record
 *
 * @return the number of bits used to encode the key
 **/
static INLINE unsigned int
get_delta_entry_key_bits(const struct delta_index_entry *entry)
{
	/*
	 * Derive keyBits by subtracting the sizes of the other two fields from
	 * the total. We don't actually use this for encoding/decoding, so it
	 * doesn't need to be super-fast. We save time where it matters by not
	 * storing it.
	 */
	return (entry->entry_bits - entry->value_bits -
		(entry->is_collision ? COLLISION_BITS : 0));
}

/**
 * Get the value field of the delta_index_entry
 *
 * @param delta_entry  The delta index record
 *
 * @return the value
 **/
static INLINE unsigned int
get_delta_entry_value(const struct delta_index_entry *delta_entry)
{
	return get_field(delta_entry->delta_zone->memory,
			 get_delta_entry_offset(delta_entry),
			 delta_entry->value_bits);
}

/**
 * Set the value field of the delta_index_entry
 *
 * @param delta_entry  The delta index record
 * @param value        The new value
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
set_delta_entry_value(const struct delta_index_entry *delta_entry,
		      unsigned int value);

/**
 * Create a new entry in the delta index
 *
 * @param delta_entry  The delta index entry that indicates the insertion point
 *                     for the new record.  For a collision entry, this is the
 *                     non-collision entry that the new entry collides with.
 *                     For a non-collision entry, this new entry is inserted
 *                     before the specified entry.
 * @param key          The key field
 * @param value        The value field
 * @param name         For collision entries, the 256 bit full name;
 *                     Otherwise null
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check put_delta_index_entry(struct delta_index_entry *delta_entry,
				       unsigned int key,
				       unsigned int value,
				       const byte *name);

/**
 * Remove an existing delta index entry, and advance to the next entry in
 * the delta list.
 *
 * @param delta_entry  On call the delta index record to remove.  After
 *                     returning, the following entry in the delta list.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
remove_delta_index_entry(struct delta_index_entry *delta_entry);

/**
 * Map a delta list number to a delta zone number
 *
 * @param delta_index  The delta index
 * @param list_number  The delta list number
 *
 * @return the zone number containing the delta list
 **/
static INLINE unsigned int
get_delta_index_zone(const struct delta_index *delta_index,
		     unsigned int list_number)
{
	return list_number / delta_index->lists_per_zone;
}

/**
 * Get the first delta list number in a zone
 *
 * @param delta_index  The delta index
 * @param zone_number  The zone number
 *
 * @return the first delta list index in the zone
 **/
unsigned int
get_delta_index_zone_first_list(const struct delta_index *delta_index,
				unsigned int zone_number);

/**
 * Get the number of delta lists in a zone
 *
 * @param delta_index  The delta index
 * @param zone_number  The zone number
 *
 * @return the number of delta lists in the zone
 **/
unsigned int
get_delta_index_zone_num_lists(const struct delta_index *delta_index,
			       unsigned int zone_number);

/**
 * Get the number of bytes used for volume index entries in a zone
 *
 * @param delta_index  The delta index
 * @param zone_number  The zone number
 *
 * @return The number of bits in use
 **/
uint64_t __must_check
get_delta_index_zone_dlist_bits_used(const struct delta_index *delta_index,
				     unsigned int zone_number);

/**
 * Get the number of bytes used for volume index entries.
 *
 * @param delta_index  The delta index
 *
 * @return The number of bits in use
 **/
uint64_t __must_check
get_delta_index_dlist_bits_used(const struct delta_index *delta_index);

/**
 * Get the number of bytes allocated for volume index entries.
 *
 * @param delta_index  The delta index
 *
 * @return The number of bits allocated
 **/
uint64_t __must_check
get_delta_index_dlist_bits_allocated(const struct delta_index *delta_index);

/**
 * Get the delta index statistics.
 *
 * @param delta_index  The delta index
 * @param stats        The statistics
 **/
void get_delta_index_stats(const struct delta_index *delta_index,
			   struct delta_index_stats *stats);

/**
 * Get the number of pages needed for an immutable delta index.
 *
 * @param num_entries       The number of entries in the index
 * @param num_lists         The number of delta lists
 * @param mean_delta        The mean delta value
 * @param num_payload_bits  The number of bits in the payload or value
 * @param bytes_per_page    The number of bytes in a page
 *
 * @return the number of pages needed for the index
 **/
unsigned int get_delta_index_page_count(unsigned int num_entries,
					unsigned int num_lists,
					unsigned int mean_delta,
					unsigned int num_payload_bits,
					size_t bytes_per_page);

/**
 * Log a delta index entry, and any error conditions related to the entry.
 *
 * @param delta_entry  The delta index entry.
 **/
void log_delta_index_entry(struct delta_index_entry *delta_entry);

#endif /* DELTAINDEX_H */
