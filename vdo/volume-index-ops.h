/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VOLUMEINDEXOPS_H
#define VOLUMEINDEXOPS_H 1

#include "compiler.h"
#include "config.h"
#include "delta-index.h"
#include "uds-threads.h"
#include "uds.h"

extern unsigned int min_volume_index_delta_lists;

struct volume_index_stats {
	size_t memory_allocated;    /* Number of bytes allocated */
	ktime_t rebalance_time;	    /* Nanoseconds spent rebalancing */
	int rebalance_count;        /* Number of memory rebalances */
	long record_count;          /* The number of records in the index */
	long collision_count;       /* The number of collision records */
	long discard_count;         /* The number of records removed */
	long overflow_count;        /* The number of UDS_OVERFLOWs detected */
	unsigned int num_lists;     /* The number of delta lists */
	long early_flushes;         /* Number of early flushes */
};

/*
 * The volume_index_record structure is used for normal index read-write
 * processing of a chunk name.  The first call must be to
 * get_volume_index_record() to find the volume index record for a chunk name.
 * This call can be followed by put_volume_index_record() to add a volume
 * index record, or by set_volume_index_record_chapter() to associate the chunk
 * name with a different chapter, or by remove_volume_index_record() to delete
 * a volume index record.
 */
struct volume_index_record {
	/* Public fields */
	uint64_t virtual_chapter;  /* Chapter where the block info is found */
	bool is_collision;         /* This record is a collision */
	bool is_found;             /* This record is the block searched for */

	/* Private fields */
	unsigned char magic;                   /* The magic number for valid */
					       /* records */
	unsigned int zone_number;              /* Zone that contains this block */
	struct volume_index *volume_index;     /* The volume index */
	struct mutex *mutex;                   /* Mutex that must be held while */
					       /* accessing this delta index */
					       /* entry; used only for a */
					       /* sampled index; otherwise is */
					       /* NULL */
	const struct uds_chunk_name *name;     /* The blockname to which this */
					       /* record refers */
	struct delta_index_entry delta_entry;  /* The delta index entry for */
					       /* this record */
};

struct volume_index {
	void (*abort_restoring_volume_index)(struct volume_index *volume_index);
	int (*finish_restoring_volume_index)(struct volume_index *volume_index,
					     struct buffered_reader **buffered_readers,
					     unsigned int num_readers);
	int (*finish_saving_volume_index)(const struct volume_index *volume_index,
					  unsigned int zone_number);
	void (*free_volume_index)(struct volume_index *volume_index);
	int (*get_volume_index_record)(struct volume_index *volume_index,
				       const struct uds_chunk_name *name,
				       struct volume_index_record *record);
	void (*get_volume_index_stats)(const struct volume_index *volume_index,
				       struct volume_index_stats *dense,
				       struct volume_index_stats *sparse);
	unsigned int (*get_volume_index_zone)(const struct volume_index *volume_index,
					      const struct uds_chunk_name *name);
	bool (*is_volume_index_sample)(const struct volume_index *volume_index,
				       const struct uds_chunk_name *name);
	uint64_t (*lookup_volume_index_name)(const struct volume_index *volume_index,
					     const struct uds_chunk_name *name);
	uint64_t (*lookup_volume_index_sampled_name)(const struct volume_index *volume_index,
						     const struct uds_chunk_name *name);
	void (*set_volume_index_open_chapter)(struct volume_index *volume_index,
					      uint64_t virtual_chapter);
	void (*set_volume_index_tag)(struct volume_index *volume_index,
				     byte tag);
	void (*set_volume_index_zone_open_chapter)(struct volume_index *volume_index,
						   unsigned int zone_number,
						   uint64_t virtual_chapter);
	int (*start_restoring_volume_index)(struct volume_index *volume_index,
					    struct buffered_reader **buffered_readers,
					    unsigned int num_readers);
	int (*start_saving_volume_index)(const struct volume_index *volume_index,
					 unsigned int zone_number,
					 struct buffered_writer *buffered_writer);
};

/**
 * Return the combined volume index stats.
 *
 * @param volume_index  The volume index
 * @param stats         Combined stats for the index
 **/
void get_volume_index_combined_stats(const struct volume_index *volume_index,
				     struct volume_index_stats *stats);

/**
 * Make a new volume index.
 *
 * @param config        The configuration of the volume index
 * @param volume_nonce  The nonce used to store the index
 * @param volume_index  Location to hold new volume index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check make_volume_index(const struct configuration *config,
				   uint64_t volume_nonce,
				   struct volume_index **volume_index);

/**
 * Compute the number of blocks required to save a volume index of a given
 * configuration.
 *
 * @param [in]  config           The configuration of a volume index
 * @param [in]  block_size       The size of a block in bytes.
 * @param [out] block_count      The resulting number of blocks.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
compute_volume_index_save_blocks(const struct configuration *config,
				 size_t block_size,
				 uint64_t *block_count);

/**
 * Restore a volume index.
 *
 * @param volume_index  The volume index
 * @param readers       The readers to read from.
 * @param num_readers   The number of readers.
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check load_volume_index(struct volume_index *volume_index,
				   struct buffered_reader **readers,
				   unsigned int num_readers);

int __must_check save_volume_index(struct volume_index *volume_index,
				   struct buffered_writer **writers,
				   unsigned int num_writers);

/**
 * Abort restoring a volume index from an input stream.
 *
 * @param volume_index  The volume index
 **/
static INLINE void
abort_restoring_volume_index(struct volume_index *volume_index)
{
	volume_index->abort_restoring_volume_index(volume_index);
}

/**
 * Finish restoring a volume index from an input stream.
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 **/
static INLINE int
finish_restoring_volume_index(struct volume_index *volume_index,
			      struct buffered_reader **buffered_readers,
			      unsigned int num_readers)
{
	return volume_index->finish_restoring_volume_index(volume_index,
							   buffered_readers,
							   num_readers);
}

/**
 * Finish saving a volume index to an output stream.  Force the writing of
 * all of the remaining data.  If an error occurred asynchronously during
 * the save operation, it will be returned here.
 *
 * @param volume_index  The volume index
 * @param zone_number   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
finish_saving_volume_index(const struct volume_index *volume_index,
			   unsigned int zone_number)
{
	return volume_index->finish_saving_volume_index(volume_index,
							zone_number);
}

/**
 * Terminate and clean up the volume index
 *
 * @param volume_index The volume index to terminate
 **/
static INLINE void free_volume_index(struct volume_index *volume_index)
{
	if (volume_index == NULL) {
		return;
	}

	volume_index->free_volume_index(volume_index);
}

/**
 * Find the volume index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * volume index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If is_found is false, then we did not find an entry for the block name.
 * Information is saved in the volume_index_record so that
 * put_volume_index_record() will insert an entry for that block name at the
 * proper place.
 *
 * If is_found is true, then we did find an entry for the block name.
 * Information is saved in the volume_index_record so that the "chapter" and
 * "is_collision" fields reflect the entry found.  Calls to
 * remove_volume_index_record() will remove the entry, calls to
 * set_volume_index_record_chapter() can modify the entry, and calls to
 * put_volume_index_record() can insert a collision record with this entry.
 *
 * @param volume_index  The volume index to search
 * @param name          The chunk name
 * @param record        Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int get_volume_index_record(struct volume_index *volume_index,
					  const struct uds_chunk_name *name,
					  struct volume_index_record *record)
{
	return volume_index->get_volume_index_record(volume_index, name,
						     record);
}

/**
 * Return the volume index stats.
 *
 * @param volume_index  The volume index
 * @param dense         Stats for the dense portion of the index
 * @param sparse        Stats for the sparse portion of the index
 **/
static INLINE void
get_volume_index_stats(const struct volume_index *volume_index,
		       struct volume_index_stats *dense,
		       struct volume_index_stats *sparse)
{
	volume_index->get_volume_index_stats(volume_index, dense, sparse);
}

/**
 * Find the volume index zone associated with a chunk name
 *
 * @param volume_index  The volume index
 * @param name          The chunk name
 *
 * @return the zone that the chunk name belongs to
 **/
static INLINE unsigned int
get_volume_index_zone(const struct volume_index *volume_index,
		      const struct uds_chunk_name *name)
{
	return volume_index->get_volume_index_zone(volume_index, name);
}

/**
 * Determine whether a given chunk name is a hook.
 *
 * @param volume_index  The volume index
 * @param name          The block name
 *
 * @return whether to use as sample
 **/
static INLINE bool
is_volume_index_sample(const struct volume_index *volume_index,
		       const struct uds_chunk_name *name)
{
	return volume_index->is_volume_index_sample(volume_index, name);
}

/**
 * Do a quick read-only lookup of the chunk name and return information
 * needed by the index code to process the chunk name.
 *
 * @param volume_index     The volume index
 * @param name             The chunk name
 *
 * @return The sparse virtual chapter, or UINT64_MAX if none
 **/
static INLINE uint64_t
lookup_volume_index_name(const struct volume_index *volume_index,
			 const struct uds_chunk_name *name)
{
	return volume_index->lookup_volume_index_name(volume_index, name);
}

/**
 * Do a quick read-only lookup of the sampled chunk name and return
 * information needed by the index code to process the chunk name.
 *
 * @param volume_index     The volume index
 * @param name             The chunk name
 *
 * @return The sparse virtual chapter, or UINT64_MAX if none
 **/
static INLINE uint64_t
lookup_volume_index_sampled_name(const struct volume_index *volume_index,
				 const struct uds_chunk_name *name)
{
	return volume_index->lookup_volume_index_sampled_name(volume_index,
							      name);
}

/**
 * Create a new record associated with a block name.
 *
 * @param record          The volume index record found by get_record()
 * @param virtual_chapter The chapter number where block info is found
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check put_volume_index_record(struct volume_index_record *record,
					 uint64_t virtual_chapter);

/**
 * Remove an existing record.
 *
 * @param record  The volume index record found by get_record()
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
remove_volume_index_record(struct volume_index_record *record);

/**
 * Set the open chapter number.  The volume index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * In normal operation, the virtual chapter number will be the next chapter
 * following the currently open chapter.  We will advance the volume index
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
 * @param volume_index     The volume index
 * @param virtual_chapter  The new open chapter number
 **/
static INLINE void
set_volume_index_open_chapter(struct volume_index *volume_index,
			      uint64_t virtual_chapter)
{
	volume_index->set_volume_index_open_chapter(volume_index,
						    virtual_chapter);
}

/**
 * Set the chapter number associated with a block name.
 *
 * @param record           The volume index record found by get_record()
 * @param virtual_chapter  The chapter number where block info is now found.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
set_volume_index_record_chapter(struct volume_index_record *record,
				uint64_t virtual_chapter);

/**
 * Set the tag value used when saving and/or restoring a volume index.
 *
 * @param volume_index  The volume index
 * @param tag           The tag value
 **/
static INLINE void set_volume_index_tag(struct volume_index *volume_index,
					byte tag)
{
	volume_index->set_volume_index_tag(volume_index, tag);
}

/**
 * Set the open chapter number on a zone.  The volume index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param volume_index     The volume index
 * @param zone_number      The zone number
 * @param virtual_chapter  The new open chapter number
 **/
static INLINE void
set_volume_index_zone_open_chapter(struct volume_index *volume_index,
				   unsigned int zone_number,
				   uint64_t virtual_chapter)
{
	volume_index->set_volume_index_zone_open_chapter(volume_index,
							 zone_number,
							 virtual_chapter);
}

/**
 * Start restoring the volume index from multiple buffered readers
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
start_restoring_volume_index(struct volume_index *volume_index,
			     struct buffered_reader **buffered_readers,
			     unsigned int num_readers)
{
	return volume_index->start_restoring_volume_index(volume_index,
							  buffered_readers,
							  num_readers);
}

/**
 * Start saving a volume index to a buffered output stream.
 *
 * @param volume_index     The volume index
 * @param zone_number      The number of the zone to save
 * @param buffered_writer  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
start_saving_volume_index(const struct volume_index *volume_index,
			  unsigned int zone_number,
			  struct buffered_writer *buffered_writer)
{
	return volume_index->start_saving_volume_index(volume_index,
						       zone_number,
						       buffered_writer);
}

#endif /* VOLUMEINDEXOPS_H */
