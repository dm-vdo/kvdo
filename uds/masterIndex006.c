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
 * $Id: //eng/uds-releases/krusty/src/uds/masterIndex006.c#24 $
 */
#include "masterIndex006.h"

#include "buffer.h"
#include "compiler.h"
#include "errors.h"
#include "hashUtils.h"
#include "logger.h"
#include "masterIndex005.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "threads.h"
#include "uds.h"

/*
 * The master index is a kept as a wrapper around 2 master index
 * implementations, one for dense chapters and one for sparse chapters.
 * Methods will be routed to one or the other, or both, depending on the
 * method and data passed in.
 *
 * The master index is divided into zones, and in normal operation there is
 * one thread operating on each zone.  Any operation that operates on all
 * the zones needs to do its operation at a safe point that ensures that
 * only one thread is operating on the master index.
 *
 * The only multithreaded operation supported by the sparse master index is
 * the lookup_master_index_name() method.  It is called by the thread that
 * assigns an index request to the proper zone, and needs to do a master
 * index query for sampled chunk names.  The zone mutexes are used to make
 * this lookup operation safe.
 */

struct master_index_zone {
	struct mutex hook_mutex; // Protects the sampled index in this zone
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct master_index6 {
	struct master_index common;             // Common master index methods
	unsigned int sparse_sample_rate;        // The sparse sample rate
	unsigned int num_zones;                 // The number of zones
	struct master_index *mi_non_hook;       // The non-hook index
	struct master_index *mi_hook;           // Hook index == sample index
	struct master_index_zone *master_zones; // The zones
};

/**
 * Determine whether a given chunk name is a hook.
 *
 * @param master_index   The master index
 * @param name           The block name
 *
 * @return whether to use as sample
 **/
static INLINE bool
is_master_index_sample_006(const struct master_index *master_index,
			   const struct uds_chunk_name *name)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	return (extract_sampling_bytes(name) % mi6->sparse_sample_rate) == 0;
}

/***********************************************************************/
/**
 * Get the subindex for the given chunk name
 *
 * @param master_index   The master index
 * @param name           The block name
 *
 * @return the subindex
 **/
static INLINE struct master_index *
get_sub_index(const struct master_index *master_index,
	      const struct uds_chunk_name *name)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	return (is_master_index_sample_006(master_index, name) ?
			mi6->mi_hook :
			mi6->mi_non_hook);
}

/***********************************************************************/
/**
 * Terminate and clean up the master index
 *
 * @param master_index The master index to terminate
 **/
static void free_master_index_006(struct master_index *master_index)
{
	if (master_index != NULL) {
		struct master_index6 *mi6 = container_of(master_index,
							 struct master_index6,
							 common);
		if (mi6->master_zones != NULL) {
			unsigned int zone;
			for (zone = 0; zone < mi6->num_zones; zone++) {
				destroy_mutex(&mi6->master_zones[zone].hook_mutex);
			}
			FREE(mi6->master_zones);
			mi6->master_zones = NULL;
		}
		if (mi6->mi_non_hook != NULL) {
			free_master_index(mi6->mi_non_hook);
			mi6->mi_non_hook = NULL;
		}
		if (mi6->mi_hook != NULL) {
			free_master_index(mi6->mi_hook);
			mi6->mi_hook = NULL;
		}
		FREE(master_index);
	}
}

/***********************************************************************/
/**
 * Constants and structures for the saved master index file.  "MI6" is for
 * master index 006, and "-XXXX" is a number to increment when the format of
 * the data changes.
 **/
enum { MAGIC_SIZE = 8 };
static const char MAGIC_MI_START[] = "MI6-0001";

struct mi006_data {
	char magic[MAGIC_SIZE]; // MAGIC_MI_START
	unsigned int sparse_sample_rate;
};

/***********************************************************************/
/**
 * Set the tag value used when saving and/or restoring a master index.
 *
 * @param master_index The master index
 * @param tag          The tag value
 **/
static void set_master_index_tag_006(struct master_index *master_index
				     __always_unused,
				     byte tag __always_unused)
{
}

/***********************************************************************/
static int __must_check encode_master_index_header(struct buffer *buffer,
						   struct mi006_data *header)
{
	int result = put_bytes(buffer, MAGIC_SIZE, MAGIC_MI_START);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, header->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) ==
					sizeof(struct mi006_data),
				 "%zu bytes of config written, of %zu expected",
				 content_length(buffer),
				 sizeof(struct mi006_data));
	return result;
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
static int
start_saving_master_index_006(const struct master_index *master_index,
			      unsigned int zone_number,
			      struct buffered_writer *buffered_writer)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	struct buffer *buffer;
	int result = make_buffer(sizeof(struct mi006_data), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}
	struct mi006_data header;
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, MAGIC_MI_START, MAGIC_SIZE);
	header.sparse_sample_rate = mi6->sparse_sample_rate;
	result = encode_master_index_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		free_buffer(&buffer);
		return result;
	}
	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(&buffer);
	if (result != UDS_SUCCESS) {
		log_warning_strerror(result,
				     "failed to write master index header");
		return result;
	}

	result = start_saving_master_index(mi6->mi_non_hook, zone_number,
					   buffered_writer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = start_saving_master_index(mi6->mi_hook, zone_number,
					   buffered_writer);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Have all the data been written while saving a master index to an output
 * stream?  If the answer is yes, it is still necessary to call
 * finish_saving_master_index(), which will return quickly.
 *
 * @param master_index  The master index
 * @param zone_number   The number of the zone to save
 *
 * @return true if all the data are written
 **/
static bool
is_saving_master_index_done_006(const struct master_index *master_index,
				unsigned int zone_number)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	return (is_saving_master_index_done(mi6->mi_non_hook, zone_number) &&
		is_saving_master_index_done(mi6->mi_hook, zone_number));
}

/***********************************************************************/
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
static int
finish_saving_master_index_006(const struct master_index *master_index,
			       unsigned int zone_number)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	int result = finish_saving_master_index(mi6->mi_non_hook, zone_number);
	if (result == UDS_SUCCESS) {
		result = finish_saving_master_index(mi6->mi_hook, zone_number);
	}
	return result;
}

/***********************************************************************/
/**
 * Abort saving a master index to an output stream.  If an error occurred
 * asynchronously during the save operation, it will be dropped.
 *
 * @param master_index  The master index
 * @param zone_number   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
abort_saving_master_index_006(const struct master_index *master_index,
			      unsigned int zone_number)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	int result = abort_saving_master_index(mi6->mi_non_hook, zone_number);
	int result2 = abort_saving_master_index(mi6->mi_hook, zone_number);
	if (result == UDS_SUCCESS) {
		result = result2;
	}
	return result;
}

/***********************************************************************/
static int __must_check decode_master_index_header(struct buffer *buffer,
						   struct mi006_data *header)
{
	int result = get_bytes_from_buffer(buffer, sizeof(header->magic),
					   &header->magic);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		get_uint32_le_from_buffer(buffer, &header->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		ASSERT_LOG_ONLY(content_length(buffer) == 0,
				"%zu bytes decoded of %zu expected",
				buffer_length(buffer) - content_length(buffer),
				buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		result = UDS_CORRUPT_COMPONENT;
	}
	return result;
}

/**
 * Start restoring the master index from multiple buffered readers
 *
 * @param master_index      The master index to restore into
 * @param buffered_readers  The buffered reader to read the master index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
start_restoring_master_index_006(struct master_index *master_index,
				 struct buffered_reader **buffered_readers,
				 int num_readers)
{
	struct master_index6 *mi6 =
		container_of(master_index, struct master_index6, common);
	int result =
		ASSERT_WITH_ERROR_CODE(master_index != NULL,
				       UDS_BAD_STATE,
				       "cannot restore to null master index");
	if (result != UDS_SUCCESS) {
		return result;
	}

	int i;
	for (i = 0; i < num_readers; i++) {
		struct buffer *buffer;
		result = make_buffer(sizeof(struct mi006_data), &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = read_from_buffered_reader(buffered_readers[i],
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(&buffer);
			return log_warning_strerror(result,
						    "failed to read master index header");
		}
		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(&buffer);
			return result;
		}
		struct mi006_data header;
		result = decode_master_index_header(buffer, &header);
		free_buffer(&buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if (memcmp(header.magic, MAGIC_MI_START, MAGIC_SIZE) != 0) {
			return log_warning_strerror(UDS_CORRUPT_COMPONENT,
						    "master index file had bad magic number");
		}
		if (i == 0) {
			mi6->sparse_sample_rate = header.sparse_sample_rate;
		} else if (mi6->sparse_sample_rate !=
			   header.sparse_sample_rate) {
			log_warning_strerror(UDS_CORRUPT_COMPONENT,
					     "Inconsistent sparse sample rate in delta index zone files: %u vs. %u",
					     mi6->sparse_sample_rate,
					     header.sparse_sample_rate);
			return UDS_CORRUPT_COMPONENT;
		}
	}

	result = start_restoring_master_index(mi6->mi_non_hook,
					      buffered_readers,
					      num_readers);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return start_restoring_master_index(mi6->mi_hook, buffered_readers,
					    num_readers);
}

/***********************************************************************/
/**
 * Have all the data been read while restoring a master index from an
 * input stream?
 *
 * @param master_index  The master index to restore into
 *
 * @return true if all the data are read
 **/
static bool
is_restoring_master_index_done_006(const struct master_index *master_index)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	return (is_restoring_master_index_done(mi6->mi_non_hook) &&
		is_restoring_master_index_done(mi6->mi_hook));
}

/***********************************************************************/
/**
 * Restore a saved delta list
 *
 * @param master_index  The master index to restore into
 * @param dlsi          The delta_list_save_info describing the delta list
 * @param data          The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
static int
restore_delta_list_to_master_index_006(struct master_index *master_index,
				       const struct delta_list_save_info *dlsi,
				       const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
	struct master_index6 *mi6 =
		container_of(master_index, struct master_index6, common);
	int result = restore_delta_list_to_master_index(mi6->mi_non_hook,
							dlsi,
							data);
	if (result != UDS_SUCCESS) {
		result = restore_delta_list_to_master_index(mi6->mi_hook,
							    dlsi,
							    data);
	}
	return result;
}

/***********************************************************************/
/**
 * Abort restoring a master index from an input stream.
 *
 * @param master_index  The master index
 **/
static void abort_restoring_master_index_006(struct master_index *master_index)
{
	struct master_index6 *mi6 =
		container_of(master_index, struct master_index6, common);
	abort_restoring_master_index(mi6->mi_non_hook);
	abort_restoring_master_index(mi6->mi_hook);
}

/***********************************************************************/
/**
 * Set the open chapter number on a zone.  The master index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param master_index     The master index
 * @param zone_number      The zone number
 * @param virtual_chapter  The new open chapter number
 **/
static void
set_master_index_zone_open_chapter_006(struct master_index *master_index,
				       unsigned int zone_number,
				       uint64_t virtual_chapter)
{
	struct master_index6 *mi6 =
		container_of(master_index, struct master_index6, common);
	set_master_index_zone_open_chapter(mi6->mi_non_hook, zone_number,
					   virtual_chapter);

	// We need to prevent a lookup_master_index_name() happening while we
	// are changing the open chapter number
	struct mutex *mutex = &mi6->master_zones[zone_number].hook_mutex;
	lock_mutex(mutex);
	set_master_index_zone_open_chapter(mi6->mi_hook, zone_number,
					   virtual_chapter);
	unlock_mutex(mutex);
}

/***********************************************************************/
/**
 * Set the open chapter number.  The master index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * @param master_index     The master index
 * @param virtual_chapter  The new open chapter number
 **/
static void
set_master_index_open_chapter_006(struct master_index *master_index,
				  uint64_t virtual_chapter)
{
	struct master_index6 *mi6 =
		container_of(master_index, struct master_index6, common);
	unsigned int zone;
	for (zone = 0; zone < mi6->num_zones; zone++) {
		set_master_index_zone_open_chapter_006(master_index, zone,
						       virtual_chapter);
	}
}

/***********************************************************************/
/**
 * Find the master index zone associated with a chunk name
 *
 * @param master_index  The master index
 * @param name          The chunk name
 *
 * @return the zone that the chunk name belongs to
 **/
static unsigned int
get_master_index_zone_006(const struct master_index *master_index,
			  const struct uds_chunk_name *name)
{
	return get_master_index_zone(get_sub_index(master_index, name), name);
}

/***********************************************************************/
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
static int
lookup_master_index_name_006(const struct master_index *master_index,
			     const struct uds_chunk_name *name,
			     struct master_index_triage *triage)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	triage->is_sample = is_master_index_sample_006(master_index, name);
	triage->in_sampled_chapter = false;
	triage->zone = get_master_index_zone_006(master_index, name);
	int result = UDS_SUCCESS;
	if (triage->is_sample) {
		struct mutex *mutex =
			&mi6->master_zones[triage->zone].hook_mutex;
		lock_mutex(mutex);
		result = lookup_master_index_sampled_name(mi6->mi_hook, name,
							  triage);
		unlock_mutex(mutex);
	}
	return result;
}

/***********************************************************************/
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
static int
lookup_master_index_sampled_name_006(const struct master_index *master_index
				     __always_unused,
				     const struct uds_chunk_name *name
				     __always_unused,
				     struct master_index_triage *triage
				     __always_unused)
{
	return ASSERT_WITH_ERROR_CODE(false, UDS_BAD_STATE,
				      "%s should not be called", __func__);
}

/***********************************************************************/
/**
 * Find the master index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * master index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If is_found is false, then we did not find an entry for the block
 * name.  Information is saved in the master_index_record so that
 * put_master_index_record() will insert an entry for that block name at
 * the proper place.
 *
 * If is_found is true, then we did find an entry for the block name.
 * Information is saved in the master_index_record so that the "chapter"
 * and "is_collision" fields reflect the entry found.
 * Calls to remove_master_index_record() will remove the entry, calls to
 * set_master_index_record_chapter() can modify the entry, and calls to
 * put_master_index_record() can insert a collision record with this
 * entry.
 *
 * @param master_index  The master index to search
 * @param name          The chunk name
 * @param record        Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static int get_master_index_record_006(struct master_index *master_index,
				       const struct uds_chunk_name *name,
				       struct master_index_record *record)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	int result;
	if (is_master_index_sample_006(master_index, name)) {
		/*
		 * We need to prevent a lookup_master_index_name() happening
		 * while we are finding the master index record.  Remember that
		 * because of lazy LRU flushing of the master index,
		 * get_master_index_record() is not a read-only operation.
		 */
		unsigned int zone = get_master_index_zone(mi6->mi_hook, name);
		struct mutex *mutex = &mi6->master_zones[zone].hook_mutex;
		lock_mutex(mutex);
		result = get_master_index_record(mi6->mi_hook, name, record);
		unlock_mutex(mutex);
		// Remember the mutex so that other operations on the
		// master_index_record can use it
		record->mutex = mutex;
	} else {
		result = get_master_index_record(mi6->mi_non_hook, name,
						 record);
	}
	return result;
}

/***********************************************************************/
/**
 * Get the number of bytes used for master index entries.
 *
 * @param master_index The master index
 *
 * @return The number of bytes in use
 **/
static size_t
get_master_index_memory_used_006(const struct master_index *master_index)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	return (get_master_index_memory_used(mi6->mi_non_hook) +
		get_master_index_memory_used(mi6->mi_hook));
}

/***********************************************************************/
/**
 * Return the master index stats.  There is only one portion of the master
 * index in this implementation, and we call it the dense portion of the
 * index.
 *
 * @param master_index  The master index
 * @param dense         Stats for the dense portion of the index
 * @param sparse        Stats for the sparse portion of the index
 **/
static void get_master_index_stats_006(const struct master_index *master_index,
				       struct master_index_stats *dense,
				       struct master_index_stats *sparse)
{
	const struct master_index6 *mi6 =
		const_container_of(master_index, struct master_index6, common);
	struct master_index_stats dummy_stats;
	get_master_index_stats(mi6->mi_non_hook, dense, &dummy_stats);
	get_master_index_stats(mi6->mi_hook, sparse, &dummy_stats);
}

/***********************************************************************/
struct split_config {
	struct configuration hook_config; // Describe hook part of the index
	struct geometry hook_geometry;
	struct configuration non_hook_config; // Describe non-hook part of the
					      // index
	struct geometry non_hook_geometry;
};

/***********************************************************************/
static int split_configuration006(const struct configuration *config,
				  struct split_config *split)
{
	int result = ASSERT_WITH_ERROR_CODE(config->geometry->sparse_chapters_per_volume != 0,
					    UDS_INVALID_ARGUMENT,
					    "cannot initialize sparse+dense master index with no sparse chapters");
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_WITH_ERROR_CODE(config->sparse_sample_rate != 0,
					UDS_INVALID_ARGUMENT,
					"cannot initialize sparse+dense master index with a sparse sample rate of %u",
					config->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Start with copies of the base configuration
	split->hook_config = *config;
	split->hook_geometry = *config->geometry;
	split->hook_config.geometry = &split->hook_geometry;
	split->non_hook_config = *config;
	split->non_hook_geometry = *config->geometry;
	split->non_hook_config.geometry = &split->non_hook_geometry;

	uint64_t sample_rate = config->sparse_sample_rate;
	uint64_t num_chapters = config->geometry->chapters_per_volume;
	uint64_t num_sparse_chapters =
		config->geometry->sparse_chapters_per_volume;
	uint64_t num_dense_chapters = num_chapters - num_sparse_chapters;
	uint64_t sample_records =
		config->geometry->records_per_chapter / sample_rate;

	// Adjust the number of records indexed for each chapter
	split->hook_geometry.records_per_chapter = sample_records;
	split->non_hook_geometry.records_per_chapter -= sample_records;

	// Adjust the number of chapters indexed
	split->hook_geometry.sparse_chapters_per_volume = 0;
	split->non_hook_geometry.sparse_chapters_per_volume = 0;
	split->non_hook_geometry.chapters_per_volume = num_dense_chapters;
	return UDS_SUCCESS;
}

/***********************************************************************/
int compute_master_index_save_bytes006(const struct configuration *config,
				       size_t *num_bytes)
{
	struct split_config split;
	int result = split_configuration006(config, &split);
	if (result != UDS_SUCCESS) {
		return result;
	}
	size_t hook_bytes, non_hook_bytes;
	result = compute_master_index_save_bytes005(&split.hook_config,
						    &hook_bytes);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = compute_master_index_save_bytes005(&split.non_hook_config,
						    &non_hook_bytes);
	if (result != UDS_SUCCESS) {
		return result;
	}
	// Saving a master index 006 needs a header plus the hook index plus
	// the non-hook index
	*num_bytes = sizeof(struct mi006_data) + hook_bytes + non_hook_bytes;
	return UDS_SUCCESS;
}

/***********************************************************************/
int make_master_index006(const struct configuration *config,
			 unsigned int num_zones,
			 uint64_t volume_nonce,
			 struct master_index **master_index)
{
	struct split_config split;
	int result = split_configuration006(config, &split);
	if (result != UDS_SUCCESS) {
		return result;
	}

	struct master_index6 *mi6;
	result = ALLOCATE(1, struct master_index6, "master index", &mi6);
	if (result != UDS_SUCCESS) {
		return result;
	}

	mi6->common.abort_restoring_master_index =
		abort_restoring_master_index_006;
	mi6->common.abort_saving_master_index = abort_saving_master_index_006;
	mi6->common.finish_saving_master_index =
		finish_saving_master_index_006;
	mi6->common.free_master_index = free_master_index_006;
	mi6->common.get_master_index_memory_used =
		get_master_index_memory_used_006;
	mi6->common.get_master_index_record = get_master_index_record_006;
	mi6->common.get_master_index_stats = get_master_index_stats_006;
	mi6->common.get_master_index_zone = get_master_index_zone_006;
	mi6->common.is_master_index_sample = is_master_index_sample_006;
	mi6->common.is_restoring_master_index_done =
		is_restoring_master_index_done_006;
	mi6->common.is_saving_master_index_done =
		is_saving_master_index_done_006;
	mi6->common.lookup_master_index_name = lookup_master_index_name_006;
	mi6->common.lookup_master_index_sampled_name =
		lookup_master_index_sampled_name_006;
	mi6->common.restore_delta_list_to_master_index =
		restore_delta_list_to_master_index_006;
	mi6->common.set_master_index_open_chapter =
		set_master_index_open_chapter_006;
	mi6->common.set_master_index_tag = set_master_index_tag_006;
	mi6->common.set_master_index_zone_open_chapter =
		set_master_index_zone_open_chapter_006;
	mi6->common.start_restoring_master_index =
		start_restoring_master_index_006;
	mi6->common.start_saving_master_index = start_saving_master_index_006;

	mi6->num_zones = num_zones;
	mi6->sparse_sample_rate = config->sparse_sample_rate;

	result = ALLOCATE(num_zones,
			  struct master_index_zone,
			  "master index zones",
			  &mi6->master_zones);
	unsigned int zone;
	for (zone = 0; zone < num_zones; zone++) {
		if (result == UDS_SUCCESS) {
			result = init_mutex(&mi6->master_zones[zone].hook_mutex);
		}
	}
	if (result != UDS_SUCCESS) {
		free_master_index_006(&mi6->common);
		return result;
	}

	result = make_master_index005(&split.non_hook_config,
				      num_zones,
				      volume_nonce,
				      &mi6->mi_non_hook);
	if (result != UDS_SUCCESS) {
		free_master_index_006(&mi6->common);
		return log_error_strerror(result,
					  "Error creating non hook master index");
	}
	set_master_index_tag(mi6->mi_non_hook, 'd');

	result = make_master_index005(&split.hook_config, num_zones,
				      volume_nonce, &mi6->mi_hook);
	if (result != UDS_SUCCESS) {
		free_master_index_006(&mi6->common);
		return log_error_strerror(result,
					  "Error creating hook master index");
	}
	set_master_index_tag(mi6->mi_hook, 's');

	*master_index = &mi6->common;
	return UDS_SUCCESS;
}
