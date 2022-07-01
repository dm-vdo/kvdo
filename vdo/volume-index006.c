// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */
#include "volume-index006.h"

#include "buffer.h"
#include "compiler.h"
#include "errors.h"
#include "hash-utils.h"
#include "logger.h"
#include "volume-index005.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds-threads.h"

/*
 * The volume index is a kept as a wrapper around 2 volume index
 * implementations, one for dense chapters and one for sparse chapters.
 * Methods will be routed to one or the other, or both, depending on the
 * method and data passed in.
 *
 * The volume index is divided into zones, and in normal operation there is
 * one thread operating on each zone.  Any operation that operates on all
 * the zones needs to do its operation at a safe point that ensures that
 * only one thread is operating on the volume index.
 *
 * The only multithreaded operation supported by the sparse volume index is
 * the lookup_volume_index_name() method.  It is called by the thread that
 * assigns an index request to the proper zone, and needs to do a volume
 * index query for sampled chunk names.  The zone mutexes are used to make
 * this lookup operation safe.
 */

struct volume_index_zone {
	struct mutex hook_mutex; /* Protects the sampled index in this zone */
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct volume_index6 {
	struct volume_index common;	  /* Common volume index methods */
	unsigned int sparse_sample_rate;  /* The sparse sample rate */
	unsigned int num_zones;           /* The number of zones */
	struct volume_index *vi_non_hook; /* The non-hook index */
	struct volume_index *vi_hook;     /* Hook index == sample index */
	struct volume_index_zone *zones;  /* The zones */
};

/**
 * Determine whether a given chunk name is a hook.
 *
 * @param volume_index   The volume index
 * @param name           The block name
 *
 * @return whether to use as sample
 **/
static INLINE bool
is_volume_index_sample_006(const struct volume_index *volume_index,
			   const struct uds_chunk_name *name)
{
	const struct volume_index6 *vi6 =
		const_container_of(volume_index, struct volume_index6, common);
	return (extract_sampling_bytes(name) % vi6->sparse_sample_rate) == 0;
}

/**
 * Get the subindex for the given chunk name
 *
 * @param volume_index   The volume index
 * @param name           The block name
 *
 * @return the subindex
 **/
static INLINE struct volume_index *
get_sub_index(const struct volume_index *volume_index,
	      const struct uds_chunk_name *name)
{
	const struct volume_index6 *vi6 =
		const_container_of(volume_index, struct volume_index6, common);
	return (is_volume_index_sample_006(volume_index, name) ?
			vi6->vi_hook :
			vi6->vi_non_hook);
}

/**
 * Terminate and clean up the volume index
 *
 * @param volume_index The volume index to terminate
 **/
static void free_volume_index_006(struct volume_index *volume_index)
{
	if (volume_index != NULL) {
		struct volume_index6 *vi6 = container_of(volume_index,
							 struct volume_index6,
							 common);
		if (vi6->zones != NULL) {
			unsigned int zone;

			for (zone = 0; zone < vi6->num_zones; zone++) {
				uds_destroy_mutex(&vi6->zones[zone].hook_mutex);
			}
			UDS_FREE(vi6->zones);
			vi6->zones = NULL;
		}
		if (vi6->vi_non_hook != NULL) {
			free_volume_index(vi6->vi_non_hook);
			vi6->vi_non_hook = NULL;
		}
		if (vi6->vi_hook != NULL) {
			free_volume_index(vi6->vi_hook);
			vi6->vi_hook = NULL;
		}
		UDS_FREE(volume_index);
	}
}

/**
 * Constants and structures for the saved volume index region. "MI6"
 * indicates volume index 006, and "-XXXX" is a number to increment
 * when the format of the data changes. The abbreviation MI6 is
 * derived from the name of a previous data structure that represented
 * the volume index region.
 **/
enum { MAGIC_SIZE = 8 };
static const char MAGIC_START[] = "MI6-0001";

struct vi006_data {
	char magic[MAGIC_SIZE]; /* MAGIC_START */
	unsigned int sparse_sample_rate;
};

/**
 * Set the tag value used when saving and/or restoring a volume index.
 *
 * @param volume_index The volume index
 * @param tag          The tag value
 **/
static void set_volume_index_tag_006(struct volume_index *volume_index
				     __always_unused,
				     byte tag __always_unused)
{
}

static int __must_check encode_volume_index_header(struct buffer *buffer,
						   struct vi006_data *header)
{
	int result = put_bytes(buffer, MAGIC_SIZE, MAGIC_START);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, header->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) ==
					sizeof(struct vi006_data),
				 "%zu bytes of config written, of %zu expected",
				 content_length(buffer),
				 sizeof(struct vi006_data));
	return result;
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
static int
start_saving_volume_index_006(const struct volume_index *volume_index,
			      unsigned int zone_number,
			      struct buffered_writer *buffered_writer)
{
	struct vi006_data header;
	const struct volume_index6 *vi6 =
		const_container_of(volume_index, struct volume_index6, common);
	struct buffer *buffer;
	int result = make_buffer(sizeof(struct vi006_data), &buffer);

	if (result != UDS_SUCCESS) {
		return result;
	}
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, MAGIC_START, MAGIC_SIZE);
	header.sparse_sample_rate = vi6->sparse_sample_rate;
	result = encode_volume_index_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		uds_log_warning_strerror(result,
					 "failed to write volume index header");
		return result;
	}

	result = start_saving_volume_index(vi6->vi_non_hook, zone_number,
					   buffered_writer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = start_saving_volume_index(vi6->vi_hook, zone_number,
					   buffered_writer);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return UDS_SUCCESS;
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
static int
finish_saving_volume_index_006(const struct volume_index *volume_index,
			       unsigned int zone_number)
{
	const struct volume_index6 *vi6 =
		const_container_of(volume_index, struct volume_index6, common);
	int result = finish_saving_volume_index(vi6->vi_non_hook, zone_number);

	if (result == UDS_SUCCESS) {
		result = finish_saving_volume_index(vi6->vi_hook, zone_number);
	}
	return result;
}

static int __must_check decode_volume_index_header(struct buffer *buffer,
						   struct vi006_data *header)
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
		result = UDS_CORRUPT_DATA;
	}
	return result;
}

/**
 * Start restoring the volume index from multiple buffered readers
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered reader to read the volume index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
start_restoring_volume_index_006(struct volume_index *volume_index,
				 struct buffered_reader **buffered_readers,
				 unsigned int num_readers)
{
	struct volume_index6 *vi6 =
		container_of(volume_index, struct volume_index6, common);
	unsigned int i;
	int result =
		ASSERT_WITH_ERROR_CODE(volume_index != NULL,
				       UDS_BAD_STATE,
				       "cannot restore to null volume index");
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < num_readers; i++) {
		struct vi006_data header;
		struct buffer *buffer;

		result = make_buffer(sizeof(struct vi006_data), &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = read_from_buffered_reader(buffered_readers[i],
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return uds_log_warning_strerror(result,
							"failed to read volume index header");
		}

		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}

		result = decode_volume_index_header(buffer, &header);
		free_buffer(UDS_FORGET(buffer));
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (memcmp(header.magic, MAGIC_START, MAGIC_SIZE) != 0) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"volume index file had bad magic number");
		}

		if (i == 0) {
			vi6->sparse_sample_rate = header.sparse_sample_rate;
		} else if (vi6->sparse_sample_rate !=
			   header.sparse_sample_rate) {
			uds_log_warning_strerror(UDS_CORRUPT_DATA,
						 "Inconsistent sparse sample rate in delta index zone files: %u vs. %u",
						 vi6->sparse_sample_rate,
						 header.sparse_sample_rate);
			return UDS_CORRUPT_DATA;
		}
	}

	result = start_restoring_volume_index(vi6->vi_non_hook,
					      buffered_readers,
					      num_readers);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return start_restoring_volume_index(vi6->vi_hook, buffered_readers,
					    num_readers);
}

/**
 * Abort restoring a volume index from an input stream.
 *
 * @param volume_index  The volume index
 **/
static void abort_restoring_volume_index_006(struct volume_index *volume_index)
{
	struct volume_index6 *vi6 =
		container_of(volume_index, struct volume_index6, common);
	abort_restoring_volume_index(vi6->vi_non_hook);
	abort_restoring_volume_index(vi6->vi_hook);
}

/**
 * Finish restoring a volume index from an input stream.
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 **/
static int
finish_restoring_volume_index_006(struct volume_index *volume_index,
				  struct buffered_reader **buffered_readers,
				  unsigned int num_readers)
{
	int result;
	struct volume_index6 *vi6 =
		container_of(volume_index, struct volume_index6, common);

	result = finish_restoring_volume_index(vi6->vi_non_hook,
					       buffered_readers,
					       num_readers);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return finish_restoring_volume_index(vi6->vi_hook,
					     buffered_readers,
					     num_readers);
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
static void
set_volume_index_zone_open_chapter_006(struct volume_index *volume_index,
				       unsigned int zone_number,
				       uint64_t virtual_chapter)
{
	struct volume_index6 *vi6 =
		container_of(volume_index, struct volume_index6, common);
	struct mutex *mutex = &vi6->zones[zone_number].hook_mutex;

	set_volume_index_zone_open_chapter(vi6->vi_non_hook, zone_number,
					   virtual_chapter);

	/*
	 * We need to prevent a lookup_volume_index_name() happening while we
	 * are changing the open chapter number
	 */
	uds_lock_mutex(mutex);
	set_volume_index_zone_open_chapter(vi6->vi_hook, zone_number,
					   virtual_chapter);
	uds_unlock_mutex(mutex);
}

/**
 * Set the open chapter number.  The volume index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * @param volume_index     The volume index
 * @param virtual_chapter  The new open chapter number
 **/
static void
set_volume_index_open_chapter_006(struct volume_index *volume_index,
				  uint64_t virtual_chapter)
{
	struct volume_index6 *vi6 =
		container_of(volume_index, struct volume_index6, common);
	unsigned int zone;

	for (zone = 0; zone < vi6->num_zones; zone++) {
		set_volume_index_zone_open_chapter_006(volume_index, zone,
						       virtual_chapter);
	}
}

/**
 * Find the volume index zone associated with a chunk name
 *
 * @param volume_index  The volume index
 * @param name          The chunk name
 *
 * @return the zone that the chunk name belongs to
 **/
static unsigned int
get_volume_index_zone_006(const struct volume_index *volume_index,
			  const struct uds_chunk_name *name)
{
	return get_volume_index_zone(get_sub_index(volume_index, name), name);
}

/**
 * Do a quick read-only lookup of the chunk name and return information
 * needed by the index code to process the chunk name.
 *
 * @param volume_index  The volume index
 * @param name          The chunk name
 *
 * @return The sparse virtual chapter, or UINT64_MAX if none
 **/
static uint64_t
lookup_volume_index_name_006(const struct volume_index *volume_index,
			     const struct uds_chunk_name *name)
{
	const struct volume_index6 *vi6 =
		const_container_of(volume_index, struct volume_index6, common);
	unsigned int zone_number =
		get_volume_index_zone_006(volume_index, name);
	struct mutex *mutex = &vi6->zones[zone_number].hook_mutex;
	uint64_t virtual_chapter;

	if (!is_volume_index_sample_006(volume_index, name)) {
		return UINT64_MAX;
	}

	uds_lock_mutex(mutex);
	virtual_chapter = lookup_volume_index_sampled_name(vi6->vi_hook, name);
	uds_unlock_mutex(mutex);

        return virtual_chapter;
}

/**
 * Do a quick read-only lookup of the sampled chunk name and return
 * information needed by the index code to process the chunk name.
 *
 * @param volume_index  The volume index
 * @param name          The chunk name
 *
 * @return The sparse virtual chapter, or UINT64_MAX if none
 **/
static uint64_t
lookup_volume_index_sampled_name_006(const struct volume_index *volume_index
				     __always_unused,
				     const struct uds_chunk_name *name
				     __always_unused)
{
	/* FIXME: This should never get called. */
	return UINT64_MAX;
}

/**
 * Find the volume index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * volume index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If is_found is false, then we did not find an entry for the block
 * name.  Information is saved in the volume_index_record so that
 * put_volume_index_record() will insert an entry for that block name at
 * the proper place.
 *
 * If is_found is true, then we did find an entry for the block name.
 * Information is saved in the volume_index_record so that the "chapter"
 * and "is_collision" fields reflect the entry found.
 * Calls to remove_volume_index_record() will remove the entry, calls to
 * set_volume_index_record_chapter() can modify the entry, and calls to
 * put_volume_index_record() can insert a collision record with this
 * entry.
 *
 * @param volume_index  The volume index to search
 * @param name          The chunk name
 * @param record        Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static int get_volume_index_record_006(struct volume_index *volume_index,
				       const struct uds_chunk_name *name,
				       struct volume_index_record *record)
{
	const struct volume_index6 *vi6 =
		const_container_of(volume_index, struct volume_index6, common);
	int result;

	if (is_volume_index_sample_006(volume_index, name)) {
		/*
		 * We need to prevent a lookup_volume_index_name() happening
		 * while we are finding the volume index record.  Remember that
		 * because of lazy LRU flushing of the volume index,
		 * get_volume_index_record() is not a read-only operation.
		 */
		unsigned int zone = get_volume_index_zone(vi6->vi_hook, name);
		struct mutex *mutex = &vi6->zones[zone].hook_mutex;

		uds_lock_mutex(mutex);
		result = get_volume_index_record(vi6->vi_hook, name, record);
		uds_unlock_mutex(mutex);
		/*
		 * Remember the mutex so that other operations on the
		 * volume_index_record can use it
		 */
		record->mutex = mutex;
	} else {
		result = get_volume_index_record(vi6->vi_non_hook, name,
						 record);
	}
	return result;
}

/**
 * Return the volume index stats.  There is only one portion of the volume
 * index in this implementation, and we call it the dense portion of the
 * index.
 *
 * @param volume_index  The volume index
 * @param dense         Stats for the dense portion of the index
 * @param sparse        Stats for the sparse portion of the index
 **/
static void get_volume_index_stats_006(const struct volume_index *volume_index,
				       struct volume_index_stats *dense,
				       struct volume_index_stats *sparse)
{
	const struct volume_index6 *vi6 =
		const_container_of(volume_index, struct volume_index6, common);
	struct volume_index_stats dummy_stats;

	get_volume_index_stats(vi6->vi_non_hook, dense, &dummy_stats);
	get_volume_index_stats(vi6->vi_hook, sparse, &dummy_stats);
}

struct split_config {
	struct configuration hook_config; /* Describe hook part of the index */
	struct geometry hook_geometry;
	struct configuration non_hook_config; /* Describe non-hook part of the */
					      /* index */
	struct geometry non_hook_geometry;
};

static int split_configuration006(const struct configuration *config,
				  struct split_config *split)
{
	uint64_t sample_rate, num_chapters, num_sparse_chapters;
	uint64_t num_dense_chapters, sample_records;
	int result = ASSERT_WITH_ERROR_CODE(config->geometry->sparse_chapters_per_volume != 0,
					    UDS_INVALID_ARGUMENT,
					    "cannot initialize sparse+dense volume index with no sparse chapters");
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_WITH_ERROR_CODE(config->sparse_sample_rate != 0,
					UDS_INVALID_ARGUMENT,
					"cannot initialize sparse+dense volume index with a sparse sample rate of %u",
					config->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Start with copies of the base configuration */
	split->hook_config = *config;
	split->hook_geometry = *config->geometry;
	split->hook_config.geometry = &split->hook_geometry;
	split->non_hook_config = *config;
	split->non_hook_geometry = *config->geometry;
	split->non_hook_config.geometry = &split->non_hook_geometry;

	sample_rate = config->sparse_sample_rate;
	num_chapters = config->geometry->chapters_per_volume;
	num_sparse_chapters = config->geometry->sparse_chapters_per_volume;
	num_dense_chapters = num_chapters - num_sparse_chapters;
	sample_records = config->geometry->records_per_chapter / sample_rate;

	/* Adjust the number of records indexed for each chapter */
	split->hook_geometry.records_per_chapter = sample_records;
	split->non_hook_geometry.records_per_chapter -= sample_records;

	/* Adjust the number of chapters indexed */
	split->hook_geometry.sparse_chapters_per_volume = 0;
	split->non_hook_geometry.sparse_chapters_per_volume = 0;
	split->non_hook_geometry.chapters_per_volume = num_dense_chapters;
	return UDS_SUCCESS;
}

int compute_volume_index_save_bytes006(const struct configuration *config,
				       size_t *num_bytes)
{
	size_t hook_bytes, non_hook_bytes;
	struct split_config split;
	int result = split_configuration006(config, &split);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = compute_volume_index_save_bytes005(&split.hook_config,
						    &hook_bytes);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = compute_volume_index_save_bytes005(&split.non_hook_config,
						    &non_hook_bytes);
	if (result != UDS_SUCCESS) {
		return result;
	}
	/*
	 * Saving a volume index 006 needs a header plus the hook index plus
	 * the non-hook index
	 */
	*num_bytes = sizeof(struct vi006_data) + hook_bytes + non_hook_bytes;
	return UDS_SUCCESS;
}

int make_volume_index006(const struct configuration *config,
			 uint64_t volume_nonce,
			 struct volume_index **volume_index)
{
	struct split_config split;
	unsigned int zone;
	struct volume_index6 *vi6;
	int result = split_configuration006(config, &split);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(1, struct volume_index6, "volume index", &vi6);
	if (result != UDS_SUCCESS) {
		return result;
	}

	vi6->common.abort_restoring_volume_index =
		abort_restoring_volume_index_006;
	vi6->common.finish_restoring_volume_index =
		finish_restoring_volume_index_006;
	vi6->common.finish_saving_volume_index =
		finish_saving_volume_index_006;
	vi6->common.free_volume_index = free_volume_index_006;
	vi6->common.get_volume_index_record = get_volume_index_record_006;
	vi6->common.get_volume_index_stats = get_volume_index_stats_006;
	vi6->common.get_volume_index_zone = get_volume_index_zone_006;
	vi6->common.is_volume_index_sample = is_volume_index_sample_006;
	vi6->common.lookup_volume_index_name = lookup_volume_index_name_006;
	vi6->common.lookup_volume_index_sampled_name =
		lookup_volume_index_sampled_name_006;
	vi6->common.set_volume_index_open_chapter =
		set_volume_index_open_chapter_006;
	vi6->common.set_volume_index_tag = set_volume_index_tag_006;
	vi6->common.set_volume_index_zone_open_chapter =
		set_volume_index_zone_open_chapter_006;
	vi6->common.start_restoring_volume_index =
		start_restoring_volume_index_006;
	vi6->common.start_saving_volume_index = start_saving_volume_index_006;

	vi6->num_zones = config->zone_count;
	vi6->sparse_sample_rate = config->sparse_sample_rate;

	result = UDS_ALLOCATE(config->zone_count,
			      struct volume_index_zone,
			      "volume index zones",
			      &vi6->zones);
	for (zone = 0; zone < config->zone_count; zone++) {
		if (result == UDS_SUCCESS) {
			result = uds_init_mutex(&vi6->zones[zone].hook_mutex);
		}
	}
	if (result != UDS_SUCCESS) {
		free_volume_index_006(&vi6->common);
		return result;
	}

	result = make_volume_index005(&split.non_hook_config,
				      volume_nonce,
				      &vi6->vi_non_hook);
	if (result != UDS_SUCCESS) {
		free_volume_index_006(&vi6->common);
		return uds_log_error_strerror(result,
					      "Error creating non hook volume index");
	}
	set_volume_index_tag(vi6->vi_non_hook, 'd');

	result = make_volume_index005(&split.hook_config,
				      volume_nonce,
				      &vi6->vi_hook);
	if (result != UDS_SUCCESS) {
		free_volume_index_006(&vi6->common);
		return uds_log_error_strerror(result,
					      "Error creating hook volume index");
	}
	set_volume_index_tag(vi6->vi_hook, 's');

	*volume_index = &vi6->common;
	return UDS_SUCCESS;
}
