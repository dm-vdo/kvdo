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
 * $Id: //eng/uds-releases/krusty/src/uds/masterIndexOps.h#16 $
 */

#ifndef MASTERINDEXOPS_H
#define MASTERINDEXOPS_H 1

#include "compiler.h"
#include "deltaIndex.h"
#include "indexComponent.h"
#include "indexConfig.h"
#include "threads.h"
#include "uds.h"

extern const struct index_component_info *const MASTER_INDEX_INFO;
extern unsigned int min_master_index_delta_lists;

struct master_index_stats {
	size_t memory_allocated;    // Number of bytes allocated
	ktime_t rebalance_time;	    // Nanoseconds spent rebalancing
	int rebalance_count;        // Number of memory rebalances
	long record_count;          // The number of records in the index
	long collision_count;       // The number of collision records
	long discard_count;         // The number of records removed
	long overflow_count;        // The number of UDS_OVERFLOWs detected
	unsigned int num_lists;     // The number of delta lists
	long early_flushes;         // Number of early flushes
};

/*
 * The master_index_triage structure is used by lookup_master_index_name(),
 * which is a read-only operation that looks at the chunk name and returns
 * some information used by the index to select the thread/queue/code_path
 * that will process the chunk.
 */
struct master_index_triage {
	uint64_t virtual_chapter;  // If in_sampled_chapter is true, then this
				   // is the chapter containing the entry for
				   // the chunk name
	unsigned int zone;         // The zone containing the chunk name
	bool is_sample;            // If true, this chunk name belongs to the
				   // sampled index
	bool in_sampled_chapter;   // If true, this chunk already has an entry
				   // in the sampled index and virtual_chapter
				   // is valid
};

/*
 * The master_index_record structure is used for normal index read-write
 * processing of a chunk name.  The first call must be to
 * get_master_index_record() to find the master index record for a chunk name.
 * This call can be followed by put_master_index_record() to add a master
 * index record, or by set_master_index_record_chapter() to associate the chunk
 * name with a different chapter, or by remove_master_index_record() to delete
 * a master index record.
 */
struct master_index_record {
	// Public fields
	uint64_t virtual_chapter;  // Chapter where the block info is found
	bool is_collision;         // This record is a collision
	bool is_found;             // This record is the block searched for

	// Private fields
	unsigned char magic;                   // The magic number for valid
					       // records
	unsigned int zone_number;              // Zone that contains this block
	struct master_index *master_index;     // The master index
	struct mutex *mutex;                   // Mutex that must be held while
					       // accessing this delta index
					       // entry; used only for a
					       // sampled index; otherwise is
					       // NULL
	const struct uds_chunk_name *name;     // The blockname to which this
					       // record refers
	struct delta_index_entry delta_entry;  // The delta index entry for
					       // this record
};

struct master_index {
	void (*abort_restoring_master_index)(struct master_index *master_index);
	int (*abort_saving_master_index)(const struct master_index *master_index,
					 unsigned int zone_number);
	int (*finish_saving_master_index)(const struct master_index *master_index,
					  unsigned int zone_number);
	void (*free_master_index)(struct master_index *master_index);
	size_t (*get_master_index_memory_used)(const struct master_index *master_index);
	int (*get_master_index_record)(struct master_index *master_index,
				       const struct uds_chunk_name *name,
				       struct master_index_record *record);
	void (*get_master_index_stats)(const struct master_index *master_index,
				       struct master_index_stats *dense,
				       struct master_index_stats *sparse);
	unsigned int (*get_master_index_zone)(const struct master_index *master_index,
					      const struct uds_chunk_name *name);
	bool (*is_master_index_sample)(const struct master_index *master_index,
				       const struct uds_chunk_name *name);
	bool (*is_restoring_master_index_done)(const struct master_index *master_index);
	bool (*is_saving_master_index_done)(const struct master_index *master_index,
					    unsigned int zone_number);
	int (*lookup_master_index_name)(const struct master_index *master_index,
					const struct uds_chunk_name *name,
					struct master_index_triage *triage);
	int (*lookup_master_index_sampled_name)(const struct master_index *master_index,
					        const struct uds_chunk_name *name,
					        struct master_index_triage *triage);
	int (*restore_delta_list_to_master_index)(struct master_index *master_index,
						  const struct delta_list_save_info *dlsi,
						  const byte data[DELTA_LIST_MAX_BYTE_COUNT]);
	void (*set_master_index_open_chapter)(struct master_index *master_index,
					      uint64_t virtual_chapter);
	void (*set_master_index_tag)(struct master_index *master_index,
				     byte tag);
	void (*set_master_index_zone_open_chapter)(struct master_index *master_index,
						   unsigned int zone_number,
						   uint64_t virtual_chapter);
	int (*start_restoring_master_index)(struct master_index *master_index,
					    struct buffered_reader **buffered_readers,
					    int num_readers);
	int (*start_saving_master_index)(const struct master_index *master_index,
					 unsigned int zone_number,
					 struct buffered_writer *buffered_writer);
};

/**
 * Return the combined master index stats.
 *
 * @param master_index  The master index
 * @param stats         Combined stats for the index
 **/
void get_master_index_combined_stats(const struct master_index *master_index,
				     struct master_index_stats *stats);

/**
 * Make a new master index.
 *
 * @param config        The configuration of the master index
 * @param num_zones     The number of zones
 * @param volume_nonce  The nonce used to store the index
 * @param master_index  Location to hold new master index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check make_master_index(const struct configuration *config,
				   unsigned int num_zones,
				   uint64_t volume_nonce,
				   struct master_index **master_index);

/**
 * Compute the number of blocks required to save a master index of a given
 * configuration.
 *
 * @param [in]  config           The configuration of a master index
 * @param [in]  block_size       The size of a block in bytes.
 * @param [out] block_count      The resulting number of blocks.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
compute_master_index_save_blocks(const struct configuration *config,
				 size_t block_size,
				 uint64_t *block_count);

/**
 * Restore a master index.  This is exposed for unit tests.
 *
 * @param readers       The readers to read from.
 * @param num_readers   The number of readers.
 * @param master_index  The master index
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check restore_master_index(struct buffered_reader **readers,
				      unsigned int num_readers,
				      struct master_index *master_index);

/**
 * Abort restoring a master index from an input stream.
 *
 * @param master_index  The master index
 **/
static INLINE void
abort_restoring_master_index(struct master_index *master_index)
{
	master_index->abort_restoring_master_index(master_index);
}

/**
 * Abort saving a master index to an output stream.  If an error occurred
 * asynchronously during the save operation, it will be dropped.
 *
 * @param master_index  The master index
 * @param zone_number   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
abort_saving_master_index(const struct master_index *master_index,
			  unsigned int zone_number)
{
	return master_index->abort_saving_master_index(master_index,
						       zone_number);
}

/**
 * Finish saving a master index to an output stream.  Force the writing of
 * all of the remaining data.  If an error occurred asynchronously during
 * the save operation, it will be returned here.
 *
 * @param master_index  The master index
 * @param zone_number   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
finish_saving_master_index(const struct master_index *master_index,
			   unsigned int zone_number)
{
	return master_index->finish_saving_master_index(master_index,
							zone_number);
}

/**
 * Terminate and clean up the master index
 *
 * @param master_index The master index to terminate
 **/
static INLINE void free_master_index(struct master_index *master_index)
{
	master_index->free_master_index(master_index);
}

/**
 * Get the number of bytes used for master index entries.
 *
 * @param master_index The master index
 *
 * @return The number of bytes in use
 **/
static INLINE size_t
get_master_index_memory_used(const struct master_index *master_index)
{
	return master_index->get_master_index_memory_used(master_index);
}

/**
 * Find the master index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * master index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If is_found is false, then we did not find an entry for the block name.
 * Information is saved in the master_index_record so that
 * put_master_index_record() will insert an entry for that block name at the
 * proper place.
 *
 * If is_found is true, then we did find an entry for the block name.
 * Information is saved in the master_index_record so that the "chapter" and
 * "is_collision" fields reflect the entry found.  Calls to
 * remove_master_index_record() will remove the entry, calls to
 * set_master_index_record_chapter() can modify the entry, and calls to
 * put_master_index_record() can insert a collision record with this entry.
 *
 * @param master_index  The master index to search
 * @param name          The chunk name
 * @param record        Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int get_master_index_record(struct master_index *master_index,
					  const struct uds_chunk_name *name,
					  struct master_index_record *record)
{
	return master_index->get_master_index_record(master_index, name,
						     record);
}

/**
 * Return the master index stats.
 *
 * @param master_index  The master index
 * @param dense         Stats for the dense portion of the index
 * @param sparse        Stats for the sparse portion of the index
 **/
static INLINE void
get_master_index_stats(const struct master_index *master_index,
		       struct master_index_stats *dense,
		       struct master_index_stats *sparse)
{
	master_index->get_master_index_stats(master_index, dense, sparse);
}

/**
 * Find the master index zone associated with a chunk name
 *
 * @param master_index  The master index
 * @param name          The chunk name
 *
 * @return the zone that the chunk name belongs to
 **/
static INLINE unsigned int
get_master_index_zone(const struct master_index *master_index,
		      const struct uds_chunk_name *name)
{
	return master_index->get_master_index_zone(master_index, name);
}

/**
 * Determine whether a given chunk name is a hook.
 *
 * @param master_index  The master index
 * @param name          The block name
 *
 * @return whether to use as sample
 **/
static INLINE bool
is_master_index_sample(const struct master_index *master_index,
		       const struct uds_chunk_name *name)
{
	return master_index->is_master_index_sample(master_index, name);
}

/**
 * Have all the data been read while restoring a master index from an input
 * stream?
 *
 * @param master_index  The master index to restore into
 *
 * @return true if all the data are read
 **/
static INLINE bool
is_restoring_master_index_done(const struct master_index *master_index)
{
	return master_index->is_restoring_master_index_done(master_index);
}

/**
 * Have all the data been written while saving a master index to an
 * output stream?  If the answer is yes, it is still necessary to call
 * finish_saving_master_index(), which will return quickly.
 *
 * @param master_index  The master index
 * @param zone_number   The number of the zone to save
 *
 * @return true if all the data are written
 **/
static INLINE bool
is_saving_master_index_done(const struct master_index *master_index,
			    unsigned int zone_number)
{
	return master_index->is_saving_master_index_done(master_index,
							 zone_number);
}

/**
 * Do a quick read-only lookup of the chunk name and return information
 * needed by the index code to process the chunk name.
 *
 * @param master_index  The master index
 * @param name          The chunk name
 * @param triage        Information about the chunk name
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int
lookup_master_index_name(const struct master_index *master_index,
			 const struct uds_chunk_name *name,
			 struct master_index_triage *triage)
{
	return master_index->lookup_master_index_name(master_index, name,
						      triage);
}

/**
 * Do a quick read-only lookup of the sampled chunk name and return
 * information needed by the index code to process the chunk name.
 *
 * @param master_index  The master index
 * @param name          The chunk name
 * @param triage        Information about the chunk name.  The zone and
 *                      is_sample fields are already filled in.  Set
 *                      in_sampled_chapter and virtual_chapter if the chunk
 *                      name is found in the index.
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int
lookup_master_index_sampled_name(const struct master_index *master_index,
				 const struct uds_chunk_name *name,
				 struct master_index_triage *triage)
{
	return master_index->lookup_master_index_sampled_name(master_index,
							      name, triage);
}

/**
 * Create a new record associated with a block name.
 *
 * @param record          The master index record found by get_record()
 * @param virtual_chapter The chapter number where block info is found
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check put_master_index_record(struct master_index_record *record,
					 uint64_t virtual_chapter);

/**
 * Remove an existing record.
 *
 * @param record  The master index record found by get_record()
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
remove_master_index_record(struct master_index_record *record);

/**
 * Restore a saved delta list
 *
 * @param master_index  The master index to restore into
 * @param dlsi          The delta_list_save_info describing the delta list
 * @param data          The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
static INLINE int
restore_delta_list_to_master_index(struct master_index *master_index,
				   const struct delta_list_save_info *dlsi,
				   const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
	return master_index->restore_delta_list_to_master_index(master_index,
								dlsi, data);
}

/**
 * Set the open chapter number.  The master index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * In normal operation, the virtual chapter number will be the next chapter
 * following the currently open chapter.  We will advance the master index
 * one chapter forward in the virtual chapter space, invalidating the
 * oldest chapter in the index and be prepared to add index entries for the
 * newly opened chapter.
 *
 * In abnormal operation we make a potentially large change to the range of
 * chapters being indexed.  This happens when we are replaying chapters or
 * rebuilding an entire index.  If we move the open chapter forward, we
 * will invalidate many chapters (potentially the entire index).  If we
 * move the open chapter backward, we invalidate any entry in the newly
 * open chapter and any higher numbered chapter (potentially the entire
 * index).
 *
 * @param master_index     The master index
 * @param virtual_chapter  The new open chapter number
 **/
static INLINE void
set_master_index_open_chapter(struct master_index *master_index,
			      uint64_t virtual_chapter)
{
	master_index->set_master_index_open_chapter(master_index,
						    virtual_chapter);
}

/**
 * Set the chapter number associated with a block name.
 *
 * @param record           The master index record found by get_record()
 * @param virtual_chapter  The chapter number where block info is now found.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
set_master_index_record_chapter(struct master_index_record *record,
				uint64_t virtual_chapter);

/**
 * Set the tag value used when saving and/or restoring a master index.
 *
 * @param master_index  The master index
 * @param tag           The tag value
 **/
static INLINE void set_master_index_tag(struct master_index *master_index,
					byte tag)
{
	master_index->set_master_index_tag(master_index, tag);
}

/**
 * Set the open chapter number on a zone.  The master index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param master_index     The master index
 * @param zone_number      The zone number
 * @param virtual_chapter  The new open chapter number
 **/
static INLINE void
set_master_index_zone_open_chapter(struct master_index *master_index,
				   unsigned int zone_number,
				   uint64_t virtual_chapter)
{
	master_index->set_master_index_zone_open_chapter(master_index,
							 zone_number,
							 virtual_chapter);
}

/**
 * Start restoring the master index from multiple buffered readers
 *
 * @param master_index      The master index to restore into
 * @param buffered_readers  The buffered readers to read the master index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
start_restoring_master_index(struct master_index *master_index,
			     struct buffered_reader **buffered_readers,
			     int num_readers)
{
	return master_index->start_restoring_master_index(master_index,
							  buffered_readers,
							  num_readers);
}

/**
 * Start saving a master index to a buffered output stream.
 *
 * @param master_index     The master index
 * @param zone_number      The number of the zone to save
 * @param buffered_writer  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
start_saving_master_index(const struct master_index *master_index,
			  unsigned int zone_number,
			  struct buffered_writer *buffered_writer)
{
	return master_index->start_saving_master_index(master_index,
						       zone_number,
						       buffered_writer);
}

#endif /* MASTERINDEXOPS_H */
