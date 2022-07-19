// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "index-layout.h"

#include <linux/murmurhash3.h>

#include "buffer.h"
#include "compiler.h"
#include "config.h"
#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "open-chapter.h"
#include "random.h"
#include "time-utils.h"
#include "volume-index-ops.h"

/*
 * The UDS layout on storage media is divided into a number of fixed-size
 * regions, the sizes of which are computed when the index is created. Every
 * header and region begins on 4K block boundary. Save regions are further
 * sub-divided into regions of their own.
 *
 * Each region has a kind and an instance number. Some kinds only have one
 * instance and therefore use RL_SOLE_INSTANCE (-1) as the instance number.
 * The RL_KIND_INDEX used to use instances to represent sub-indices; now,
 * however there is only ever one sub-index and therefore one instance. A save
 * region holds a clean shutdown. The instances determine which available save
 * slot is used. The RL_KIND_VOLUME_INDEX uses instances to record which zone
 * is being saved.
 *
 * Every region header has a type and version. 
 *
 *     +-+-+---------+--------+--------+-+
 *     | | |   I N D E X  0   101, 0   | |
 *     |H|C+---------+--------+--------+S|
 *     |D|f| Volume  | Save   | Save   |e|
 *     |R|g| Region  | Region | Region |a|
 *     | | | 201, -1 | 202, 0 | 202, 1 |l|
 *     +-+-+--------+---------+--------+-+
 *
 * The header contains the encoded region layout table as well as some index
 * configuration data. The sub-index region and its subdivisions are maintained
 * in the same table.
 *
 * There are two save regions to preserve the old state in case saving the new
 * state is incomplete. They are used in alternation.  Each save region is
 * further divided into sub-regions.
 *
 *     +-+-----+------+------+-----+-----+
 *     |H| IPM | MI   | MI   |     | OC  |
 *     |D|     | zone | zone | ... |     |
 *     |R| 301 | 302  | 302  |     | 303 |
 *     | | -1  |  0   |  1   |     | -1  |
 *     +-+-----+------+------+-----+-----+
 *
 * The header contains the encoded region layout table as well as index state
 * data for that save. Each save also has a unique nonce.
 */

enum {
	MAGIC_SIZE = 32,
	NONCE_INFO_SIZE = 32,
	MAX_SAVES = 2,
};

enum region_kind {
	RL_KIND_EMPTY = 0, /* uninitialized or scrapped */
	RL_KIND_HEADER = 1,
	RL_KIND_CONFIG = 100,
	RL_KIND_INDEX = 101,
	RL_KIND_SEAL = 102,
	RL_KIND_VOLUME = 201,
	RL_KIND_SAVE = 202,
	RL_KIND_INDEX_PAGE_MAP = 301,
	RL_KIND_VOLUME_INDEX = 302,
	RL_KIND_OPEN_CHAPTER = 303,
};

/* Some region types are historical and are no longer used. */
enum region_type {
	RH_TYPE_FREE = 0, /* unused */
	RH_TYPE_SUPER = 1,
	RH_TYPE_SAVE = 2,
	RH_TYPE_CHECKPOINT = 3, /* unused */
	RH_TYPE_UNSAVED = 4,
};

enum {
	RL_SOLE_INSTANCE = 65535,
};

/*
 * Super block version 2 is the first released version.
 *
 * Super block version 3 is the normal version used from RHEL 8.2 onwards.
 *
 * Super block versions 4 through 6 were incremental development versions and
 * are not supported.
 *
 * Super block version 7 is used for volumes which have been reduced in size by
 * one chapter in order to make room to prepend LVM metadata to a volume
 * originally created without lvm. This allows the index to retain most its
 * deduplication records.
 */
enum {
	SUPER_VERSION_MINIMUM = 3,
	SUPER_VERSION_CURRENT = 3,
	SUPER_VERSION_MAXIMUM = 7,
};

static const byte LAYOUT_MAGIC[MAGIC_SIZE] = "*ALBIREO*SINGLE*FILE*LAYOUT*001*";
static const uint64_t REGION_MAGIC = 0x416c6252676e3031; /* 'AlbRgn01' */

struct region_header {
	uint64_t magic; 
	uint64_t region_blocks;
	uint16_t type;
	/* Currently always version 1 */
	uint16_t version;
	uint16_t region_count;
	uint16_t payload;
};

struct layout_region {
	uint64_t start_block;
	uint64_t block_count;
	uint32_t __unused;
	uint16_t kind;
	uint16_t instance;
};

struct region_table {
	struct region_header header;
	struct layout_region regions[];
};

struct index_save_data {
	uint64_t timestamp;
	uint64_t nonce;
	/* Currently always version 1 */
	uint32_t version;
	uint32_t unused__;
};

struct index_state_version {
	int32_t signature;
	int32_t version_id;
};

static const struct index_state_version INDEX_STATE_VERSION_301 = {
	.signature  = -1,
	.version_id = 301,
};

struct index_state_data301 {
	struct index_state_version version;
	uint64_t newest_chapter;
	uint64_t oldest_chapter;
	uint64_t last_save;
	uint32_t unused;
	uint32_t padding;
};

struct index_save_layout {
	unsigned int zone_count;
	struct layout_region index_save;
	struct layout_region header;
	struct layout_region index_page_map;
	struct layout_region free_space;
	struct layout_region volume_index_zones[MAX_ZONES];
	struct layout_region open_chapter;
	struct index_save_data save_data;
	struct index_state_data301 state_data;
};

struct sub_index_layout {
	uint64_t nonce;
	struct layout_region sub_index;
	struct layout_region volume;
	struct index_save_layout *saves;
};

struct super_block_data {
	byte magic_label[MAGIC_SIZE];
	byte nonce_info[NONCE_INFO_SIZE];
	uint64_t nonce;
	uint32_t version;
	uint32_t block_size;
	uint16_t index_count;
	uint16_t max_saves;
	/* Padding reflects a blank field on permanent storage */
	byte padding[4];
	uint64_t open_chapter_blocks;
	uint64_t page_map_blocks;
	uint64_t volume_offset;
	uint64_t start_offset;
};

struct index_layout {
	struct io_factory *factory;
	size_t factory_size;
	off_t offset;
	struct super_block_data super;
	struct layout_region header;
	struct layout_region config;
	struct sub_index_layout index;
	struct layout_region seal;
	uint64_t total_blocks;
};

struct save_layout_sizes {
	unsigned int save_count;
	size_t block_size;
	uint64_t volume_blocks;
	uint64_t volume_index_blocks;
	uint64_t page_map_blocks;
	uint64_t open_chapter_blocks;
	uint64_t save_blocks;
	uint64_t sub_index_blocks;
	uint64_t total_blocks;
	size_t total_size;
};

static INLINE bool is_converted_super_block(struct super_block_data *super)
{
	return (super->version == 7);
}

static int __must_check compute_sizes(const struct configuration *config,
				      struct save_layout_sizes *sls)
{
	int result;
	struct geometry *geometry = config->geometry;

	if ((geometry->bytes_per_page % UDS_BLOCK_SIZE) != 0) {
		return uds_log_error_strerror(UDS_INCORRECT_ALIGNMENT,
					      "page size not a multiple of block size");
	}

	memset(sls, 0, sizeof(*sls));

	sls->save_count = MAX_SAVES;
	sls->block_size = UDS_BLOCK_SIZE;
	sls->volume_blocks = geometry->bytes_per_volume / sls->block_size;

	result = compute_volume_index_save_blocks(config,
						  sls->block_size,
						  &sls->volume_index_blocks);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot compute index save size");
	}

	sls->page_map_blocks =
		DIV_ROUND_UP(compute_index_page_map_save_size(geometry),
			     sls->block_size);
	sls->open_chapter_blocks =
		DIV_ROUND_UP(compute_saved_open_chapter_size(geometry),
			     sls->block_size);
	sls->save_blocks =
		1 + (sls->volume_index_blocks + sls->page_map_blocks +
		     sls->open_chapter_blocks);
	sls->sub_index_blocks =
		sls->volume_blocks + (sls->save_count * sls->save_blocks);
	sls->total_blocks = 3 + sls->sub_index_blocks;
	sls->total_size = sls->total_blocks * sls->block_size;

	return UDS_SUCCESS;
}

int uds_compute_index_size(const struct uds_parameters *parameters,
			   uint64_t *index_size)
{
	int result;
	struct configuration *index_config;
	struct save_layout_sizes sizes;

	if (index_size == NULL) {
		uds_log_error("Missing output size pointer");
		return -EINVAL;
	}

	result = make_configuration(parameters, &index_config);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "cannot compute index size");
		return uds_map_to_system_error(result);
	}

	result = compute_sizes(index_config, &sizes);
	free_configuration(index_config);
	if (result != UDS_SUCCESS) {
		return uds_map_to_system_error(result);
	}

	*index_size = sizes.total_size;
	return UDS_SUCCESS;
}

/* Create unique data using the current time and a pseudorandom number. */
static void create_unique_nonce_data(byte *buffer)
{
	ktime_t now = current_time_ns(CLOCK_REALTIME);
	uint32_t rand = random_in_range(1, (1 << 30) - 1);
	size_t offset = 0;

	memcpy(buffer + offset, &now, sizeof(now));
	offset += sizeof(now);
	memcpy(buffer + offset, &rand, sizeof(rand));
	offset += sizeof(rand);
	while (offset < NONCE_INFO_SIZE) {
		size_t len = min(NONCE_INFO_SIZE - offset, offset);

		memcpy(buffer + offset, buffer, len);
		offset += len;
	}
}

static uint64_t hash_stuff(uint64_t start, const void *data, size_t len)
{
	uint32_t seed = start ^ (start >> 27);
	byte hash_buffer[16];

	murmurhash3_128(data, len, seed, hash_buffer);
	return get_unaligned_le64(hash_buffer + 4);
}

/* Generate a primary nonce from the provided data. */
static uint64_t generate_primary_nonce(const void *data, size_t len)
{
	return hash_stuff(0xa1b1e0fc, data, len);
}

/*
 * Deterministically generate a secondary nonce from an existing nonce and some
 * arbitrary data by hashing the original nonce and the data to produce a new
 * nonce.
 */
static uint64_t
generate_secondary_nonce(uint64_t nonce, const void *data, size_t len)
{
	return hash_stuff(nonce + 1, data, len);
}

static int __must_check
open_layout_reader(struct index_layout *layout,
		   struct layout_region *lr,
		   off_t offset,
		   struct buffered_reader **reader_ptr)
{
	off_t start = (lr->start_block + offset) * layout->super.block_size;
	size_t size = lr->block_count * layout->super.block_size;

	return open_uds_buffered_reader(layout->factory,
					start,
					size,
					reader_ptr);
}

static int open_region_reader(struct index_layout *layout,
			      struct layout_region *region,
			      struct buffered_reader **reader_ptr)
{
	return open_layout_reader(layout,
				  region,
				  -layout->super.start_offset,
				  reader_ptr);
}

static int __must_check
open_layout_writer(struct index_layout *layout,
		   struct layout_region *lr,
		   off_t offset,
		   struct buffered_writer **writer_ptr)
{
	off_t start = (lr->start_block + offset) * layout->super.block_size;
	size_t size = lr->block_count * layout->super.block_size;

	return open_uds_buffered_writer(layout->factory,
					start,
					size,
					writer_ptr);
}

static int open_region_writer(struct index_layout *layout,
			      struct layout_region *region,
			      struct buffered_writer **writer_ptr)
{
	return open_layout_writer(layout,
				  region,
				  -layout->super.start_offset,
				  writer_ptr);
}

static int __must_check
decode_region_header(struct buffer *buffer, struct region_header *header)
{
	int result;

	result = get_uint64_le_from_buffer(buffer, &header->magic);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &header->region_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint16_le_from_buffer(buffer, &header->type);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint16_le_from_buffer(buffer, &header->version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint16_le_from_buffer(buffer, &header->region_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return get_uint16_le_from_buffer(buffer, &header->payload);
}

static int __must_check
decode_layout_region(struct buffer *buffer, struct layout_region *region)
{
	int result;

	result = get_uint64_le_from_buffer(buffer, &region->start_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &region->block_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = skip_forward(buffer, sizeof(uint32_t));
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint16_le_from_buffer(buffer, &region->kind);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return get_uint16_le_from_buffer(buffer, &region->instance);
}

static int __must_check load_region_table(struct buffered_reader *reader,
					  struct region_table **table_ptr)
{
	int result;
	unsigned int i;
	struct region_header header;
	struct region_table *table;
	struct buffer *buffer;

	result = make_buffer(sizeof(struct region_header), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return uds_log_error_strerror(result,
					      "cannot read region table header");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = decode_region_header(buffer, &header);
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (header.magic != REGION_MAGIC) {
		return UDS_NO_INDEX;
	}

	if (header.version != 1) {
		return uds_log_error_strerror(UDS_UNSUPPORTED_VERSION,
					      "unknown region table version %hu",
					      header.version);
	}

	result = UDS_ALLOCATE_EXTENDED(struct region_table,
				       header.region_count,
				       struct layout_region,
				       "single file layout region table",
				       &table);
	if (result != UDS_SUCCESS) {
		return result;
	}

	table->header = header;
	result = make_buffer(header.region_count * sizeof(struct layout_region),
			     &buffer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return result;
	}

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		free_buffer(UDS_FORGET(buffer));
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "cannot read region table layouts");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	for (i = 0; i < header.region_count; i++) {
		result = decode_layout_region(buffer, &table->regions[i]);
		if (result != UDS_SUCCESS) {
			UDS_FREE(table);
			free_buffer(UDS_FORGET(buffer));
			return result;
		}
	}

	free_buffer(UDS_FORGET(buffer));
	*table_ptr = table;
	return UDS_SUCCESS;
}

static int __must_check
decode_super_block_data(struct buffer *buffer, struct super_block_data *super)
{
	int result;

	result = get_bytes_from_buffer(buffer, MAGIC_SIZE, super->magic_label);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_bytes_from_buffer(buffer,
				       NONCE_INFO_SIZE,
				       super->nonce_info);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &super->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer, &super->version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer, &super->block_size);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint16_le_from_buffer(buffer, &super->index_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint16_le_from_buffer(buffer, &super->max_saves);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = skip_forward(buffer, 4); /* aligment */
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &super->open_chapter_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &super->page_map_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (is_converted_super_block(super)) {
		result = get_uint64_le_from_buffer(buffer,
						   &super->volume_offset);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = get_uint64_le_from_buffer(buffer,
						   &super->start_offset);
		if (result != UDS_SUCCESS) {
			return result;
		}
	} else {
		super->volume_offset = 0;
		super->start_offset = 0;
	}

	return UDS_SUCCESS;
}

static int __must_check read_super_block_data(struct buffered_reader *reader,
					      struct index_layout *layout,
					      size_t saved_size)
{
	int result;
	struct super_block_data *super = &layout->super;
	struct buffer *buffer;

	if (sizeof(super->magic_label) != MAGIC_SIZE) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "super block magic label size incorrect");
	}

	result = make_buffer(saved_size, &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return uds_log_error_strerror(result,
					      "cannot read region table header");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = decode_super_block_data(buffer, super);
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot read super block data");
	}

	if (memcmp(super->magic_label, LAYOUT_MAGIC, MAGIC_SIZE) != 0) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "unknown superblock magic label");
	}

	if ((super->version < SUPER_VERSION_MINIMUM) ||
	    (super->version == 4) ||
	    (super->version == 5) ||
	    (super->version == 6) ||
	    (super->version > SUPER_VERSION_MAXIMUM)) {
		return uds_log_error_strerror(UDS_UNSUPPORTED_VERSION,
					      "unknown superblock version number %u",
					      super->version);
	}

	if (super->volume_offset < super->start_offset) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "inconsistent offsets (start %llu, volume %llu)",
					      (unsigned long long) super->start_offset,
					      (unsigned long long) super->volume_offset);
	}

	/* Sub-indexes are no longer used but the layout retains this field. */
	if (super->index_count != 1) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "invalid subindex count %u",
					      super->index_count);
	}

	if (generate_primary_nonce(super->nonce_info,
				   sizeof(super->nonce_info)) != super->nonce) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "inconsistent superblock nonce");
	}

	return UDS_SUCCESS;
}

static void define_sub_index_nonce(struct index_layout *layout)
{
	struct sub_index_nonce_data {
		uint64_t offset;
		uint16_t index_id;
	};
	struct sub_index_layout *sil = &layout->index;
	uint64_t primary_nonce = layout->super.nonce;
	byte buffer[sizeof(struct sub_index_nonce_data)] = { 0 };
	size_t offset = 0;

	encode_uint64_le(buffer, &offset, sil->sub_index.start_block);
	encode_uint16_le(buffer, &offset, 0);
	sil->nonce = generate_secondary_nonce(primary_nonce,
					      buffer,
					      sizeof(buffer));
	if (sil->nonce == 0) {
		sil->nonce = generate_secondary_nonce(~primary_nonce + 1,
						      buffer,
						      sizeof(buffer));
	}
}

static int __must_check verify_region(struct layout_region *lr,
				      uint64_t start_block,
				      enum region_kind kind,
				      unsigned int instance)
{
	if (lr->start_block != start_block) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "incorrect layout region offset");
	}

	if ((lr->kind != kind)) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "incorrect layout region kind");
	}

	if (lr->instance != instance) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "incorrect layout region instance");
	}

	return UDS_SUCCESS;
}

static int __must_check verify_sub_index(struct index_layout *layout,
					 uint64_t start_block,
					 struct region_table *table)
{
	int result;
	unsigned int i;
	struct sub_index_layout *sil = &layout->index;
	uint64_t next_block = start_block;

	sil->sub_index = table->regions[2];
	result = verify_region(&sil->sub_index,
			       next_block,
			       RL_KIND_INDEX,
			       0);
	if (result != UDS_SUCCESS) {
		return result;
	}

	define_sub_index_nonce(layout);

	sil->volume = table->regions[3];
	result = verify_region(&sil->volume,
			       next_block,
			       RL_KIND_VOLUME,
			       RL_SOLE_INSTANCE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	next_block += sil->volume.block_count + layout->super.volume_offset;

	for (i = 0; i < layout->super.max_saves; i++) {
		sil->saves[i].index_save = table->regions[i + 4];
		result = verify_region(&sil->saves[i].index_save,
				       next_block,
				       RL_KIND_SAVE,
				       i);
		if (result != UDS_SUCCESS) {
			return result;
		}

		next_block += sil->saves[i].index_save.block_count;
	}

	next_block -= layout->super.volume_offset;
	if (next_block != start_block + sil->sub_index.block_count) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "sub index region does not span all saves");
	}

	return UDS_SUCCESS;
}

static int __must_check reconstitute_layout(struct index_layout *layout,
					    struct region_table *table,
					    uint64_t first_block)
{
	int result;
	uint64_t next_block = first_block;

	result = UDS_ALLOCATE(layout->super.max_saves,
			      struct index_save_layout,
			      __func__,
			      &layout->index.saves);
	if (result != UDS_SUCCESS) {
		return result;
	}

	layout->total_blocks = table->header.region_blocks;

	layout->header = table->regions[0];
	result = verify_region(&layout->header,
			       next_block++,
			       RL_KIND_HEADER,
			       RL_SOLE_INSTANCE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	layout->config = table->regions[1];
	result = verify_region(&layout->config,
			       next_block++,
			       RL_KIND_CONFIG,
			       RL_SOLE_INSTANCE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = verify_sub_index(layout, next_block, table);
	if (result != UDS_SUCCESS) {
		return result;
	}
	next_block += layout->index.sub_index.block_count;

	layout->seal = table->regions[table->header.region_count - 1];
	result = verify_region(&layout->seal,
			       next_block + layout->super.volume_offset,
			       RL_KIND_SEAL,
			       RL_SOLE_INSTANCE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (++next_block != (first_block + layout->total_blocks)) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "layout table does not span total blocks");
	}

	return UDS_SUCCESS;
}

static int __must_check load_super_block(struct index_layout *layout,
					 size_t block_size,
					 uint64_t first_block,
					 struct buffered_reader *reader)
{
	int result;
	struct region_table *table = NULL;
	struct super_block_data *super = &layout->super;

	result = load_region_table(reader, &table);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (table->header.type != RH_TYPE_SUPER) {
		UDS_FREE(table);
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "not a superblock region table");
	}

	result = read_super_block_data(reader, layout, table->header.payload);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return uds_log_error_strerror(result,
					      "unknown superblock format");
	}

	if (super->block_size != block_size) {
		UDS_FREE(table);
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "superblock saved block_size %u differs from supplied block_size %zu",
					      super->block_size,
					      block_size);
	}

	first_block -= (super->volume_offset - super->start_offset);
	result = reconstitute_layout(layout, table, first_block);
	UDS_FREE(table);
	return result;
}

static int __must_check
decode_index_save_data(struct buffer *buffer,
		       struct index_save_data *save_data)
{
	int result;

	result = get_uint64_le_from_buffer(buffer, &save_data->timestamp);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &save_data->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &save_data->version);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return skip_forward(buffer, sizeof(uint32_t));
}

static int decode_index_state_data(struct buffer *buffer,
				   struct index_state_data301 *state_data)
{
	int result;
	struct index_state_version file_version;

	result = get_int32_le_from_buffer(buffer, &file_version.signature);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_int32_le_from_buffer(buffer, &file_version.version_id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if ((file_version.signature != INDEX_STATE_VERSION_301.signature) ||
	    (file_version.version_id != INDEX_STATE_VERSION_301.version_id)) {
		return uds_log_error_strerror(UDS_UNSUPPORTED_VERSION,
					      "index state version %d,%d is unsupported",
					      file_version.signature,
					      file_version.version_id);
	}

	result = get_uint64_le_from_buffer(buffer,
					   &state_data->newest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer,
					   &state_data->oldest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &state_data->last_save);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = skip_forward(buffer, sizeof(uint32_t));
	if (result != UDS_SUCCESS) {
		return result;
	}

	return skip_forward(buffer, sizeof(uint32_t));
}

static int __must_check read_index_save_data(struct buffered_reader *reader,
					     struct index_save_layout *isl,
					     size_t saved_size)
{
	int result;
	struct buffer *buffer = NULL;
	uint16_t payload_size = (sizeof(struct index_save_data) +
				 sizeof(struct index_state_data301));

	if (saved_size != payload_size) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "unexpected index save data size %zu",
					      saved_size);
	}

	result = make_buffer(payload_size, &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return uds_log_error_strerror(result,
					      "cannot read index save data");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = decode_index_save_data(buffer, &isl->save_data);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	if (isl->save_data.version > 1) {
		free_buffer(UDS_FORGET(buffer));
		return uds_log_error_strerror(UDS_UNSUPPORTED_VERSION,
					      "unknown index save version number %u",
					      isl->save_data.version);
	}

	result = decode_index_state_data(buffer, &isl->state_data);
	free_buffer(UDS_FORGET(buffer));
	return result;
}

static void populate_index_save_layout(struct index_save_layout *isl,
				       struct super_block_data *super)
{
	unsigned int z;
	uint64_t free_blocks;
	uint64_t volume_index_blocks;
	uint64_t next_block = isl->index_save.start_block;

	isl->header = (struct layout_region) {
		.start_block = next_block++,
		.block_count = 1,
		.kind = RL_KIND_HEADER,
		.instance = RL_SOLE_INSTANCE,
	};

	isl->index_page_map = (struct layout_region) {
		.start_block = next_block,
		.block_count = super->page_map_blocks,
		.kind = RL_KIND_INDEX_PAGE_MAP,
		.instance = RL_SOLE_INSTANCE,
	};
	next_block += super->page_map_blocks;

	free_blocks = (isl->index_save.block_count - 1 -
		       super->page_map_blocks -
		       super->open_chapter_blocks);
	volume_index_blocks = free_blocks / isl->zone_count;
	for (z = 0; z < isl->zone_count; ++z) {
		isl->volume_index_zones[z] = (struct layout_region) {
			.start_block = next_block,
			.block_count = volume_index_blocks,
			.kind = RL_KIND_VOLUME_INDEX,
			.instance = z,
		};

		next_block += volume_index_blocks;
		free_blocks -= volume_index_blocks;
	}

	isl->open_chapter = (struct layout_region) {
		.start_block = next_block,
		.block_count = super->open_chapter_blocks,
		.kind = RL_KIND_OPEN_CHAPTER,
		.instance = RL_SOLE_INSTANCE,
	};

	next_block += super->open_chapter_blocks;

	isl->free_space = (struct layout_region) {
		.start_block = next_block,
		.block_count = free_blocks,
		.kind = RL_KIND_EMPTY,
		.instance = RL_SOLE_INSTANCE,
	};
}

static int __must_check reconstruct_index_save(struct index_save_layout *isl,
					       struct region_table *table)
{
	int result;
	unsigned int z;
	struct layout_region *last_region;
	uint64_t next_block = isl->index_save.start_block;
	uint64_t last_block = next_block + isl->index_save.block_count;

	isl->zone_count = table->header.region_count - 3;

	last_region = &table->regions[table->header.region_count - 1];
	if (last_region->kind == RL_KIND_EMPTY) {
		isl->free_space = *last_region; 
		isl->zone_count--;
	} else {
		isl->free_space = (struct layout_region) {
			.start_block = last_block,
			.block_count = 0,
			.kind = RL_KIND_EMPTY,
			.instance = RL_SOLE_INSTANCE,
		};
	}

	isl->header = table->regions[0];
	result = verify_region(&isl->header,
			       next_block++,
			       RL_KIND_HEADER,
			       RL_SOLE_INSTANCE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	isl->index_page_map = table->regions[1];
	result = verify_region(&isl->index_page_map,
			       next_block,
			       RL_KIND_INDEX_PAGE_MAP,
			       RL_SOLE_INSTANCE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	next_block += isl->index_page_map.block_count;

	for (z = 0; z < isl->zone_count; z++) {
		isl->volume_index_zones[z] = table->regions[z + 2];
		result = verify_region(&isl->volume_index_zones[z],
				       next_block,
				       RL_KIND_VOLUME_INDEX,
				       z);
		if (result != UDS_SUCCESS) {
			return result;
		}

		next_block += isl->volume_index_zones[z].block_count;
	}

	isl->open_chapter = table->regions[isl->zone_count + 2];
	result = verify_region(&isl->open_chapter,
			       next_block,
			       RL_KIND_OPEN_CHAPTER,
			       RL_SOLE_INSTANCE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	next_block += isl->open_chapter.block_count;

	result = verify_region(&isl->free_space,
			       next_block,
			       RL_KIND_EMPTY,
			       RL_SOLE_INSTANCE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	next_block += isl->free_space.block_count;
	if (next_block != last_block) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "index save layout table incomplete");
	}

	return UDS_SUCCESS;
}

static void reset_index_save_layout(struct index_save_layout *isl,
				    uint64_t page_map_blocks)
{
	uint64_t free_blocks;
	uint64_t next_block = isl->index_save.start_block;

	isl->zone_count = 0;
	memset(&isl->save_data, 0, sizeof(isl->save_data));

	isl->header = (struct layout_region) {
		.start_block = next_block++,
		.block_count = 1,
		.kind = RL_KIND_HEADER,
		.instance = RL_SOLE_INSTANCE,
	};

	isl->index_page_map = (struct layout_region) {
		.start_block = next_block,
		.block_count = page_map_blocks,
		.kind = RL_KIND_INDEX_PAGE_MAP,
		.instance = RL_SOLE_INSTANCE,
	};

	next_block += page_map_blocks;

	free_blocks = isl->index_save.block_count - page_map_blocks - 1;
	isl->free_space = (struct layout_region) {
		.start_block = next_block,
		.block_count = free_blocks,
		.kind = RL_KIND_EMPTY,
		.instance = RL_SOLE_INSTANCE,
	};
}

static int __must_check load_index_save(struct index_save_layout *isl,
					struct buffered_reader *reader,
					unsigned int instance)
{
	int result;
	struct region_table *table = NULL;

	result = load_region_table(reader, &table);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot read index save %u header",
					      instance);
	}

	if (table->header.region_blocks != isl->index_save.block_count) {
		uint64_t region_blocks = table->header.region_blocks;

		UDS_FREE(table);
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "unexpected index save %u region block count %llu",
					      instance,
					      (unsigned long long) region_blocks);
	}

	if (table->header.type == RH_TYPE_UNSAVED) {
		UDS_FREE(table);
		reset_index_save_layout(isl, 0);
		return UDS_SUCCESS;
	}


	if (table->header.type != RH_TYPE_SAVE) {
		UDS_FREE(table);
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "unexpected index save %u header type %u",
					      instance,
					      table->header.type);
	}

	result = read_index_save_data(reader, isl, table->header.payload);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return uds_log_error_strerror(result,
					      "unknown index save %u data format",
					      instance);
	}

	result = reconstruct_index_save(isl, table);
	UDS_FREE(table);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot reconstruct index save %u",
					      instance);
	}

	return UDS_SUCCESS;
}

static int __must_check load_sub_index_regions(struct index_layout *layout)
{
	int result;
	unsigned int j;
	struct index_save_layout *isl;
	struct buffered_reader *reader;

	for (j = 0; j < layout->super.max_saves; ++j) {
		isl = &layout->index.saves[j];
		result = open_region_reader(layout,
					    &isl->index_save,
					    &reader);

		if (result != UDS_SUCCESS) {
			uds_log_error_strerror(result,
					       "cannot get reader for index 0 save %u",
					       j);
			return result;
		}

		result = load_index_save(isl, reader, j);
		free_buffered_reader(reader);
		if (result != UDS_SUCCESS) {
			/* Another save slot might be valid. */
			reset_index_save_layout(isl, 0);
			continue;
		}
	}

	return UDS_SUCCESS;
}

static int __must_check
verify_uds_index_config(struct index_layout *layout,
			struct configuration *config)
{
	int result;
	struct buffered_reader *reader = NULL;
	uint64_t offset = layout->super.volume_offset -
		layout->super.start_offset;

	result = open_layout_reader(layout,
				    &layout->config,
				    offset,
				    &reader);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "failed to open config reader");
	}

	result = validate_config_contents(reader, config);
	if (result != UDS_SUCCESS) {
		free_buffered_reader(reader);
		return uds_log_error_strerror(result,
					      "failed to read config region");
	}
	free_buffered_reader(reader);
	return UDS_SUCCESS;
}

static int load_index_layout(struct index_layout *layout,
			     struct configuration *config)
{
	int result;
	struct buffered_reader *reader;

	result = open_uds_buffered_reader(layout->factory,
					  layout->offset,
					  UDS_BLOCK_SIZE,
					  &reader);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "unable to read superblock");
	}

	result = load_super_block(layout,
				  UDS_BLOCK_SIZE,
				  layout->offset / UDS_BLOCK_SIZE,
				  reader);
	free_buffered_reader(reader);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = load_sub_index_regions(layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return verify_uds_index_config(layout, config);
}

static void generate_super_block_data(struct save_layout_sizes *sls,
				      struct super_block_data *super)
{
	memset(super, 0, sizeof(*super));
	memcpy(super->magic_label, LAYOUT_MAGIC, MAGIC_SIZE);
	create_unique_nonce_data(super->nonce_info);

	super->nonce = generate_primary_nonce(super->nonce_info,
					      sizeof(super->nonce_info));
	super->version = SUPER_VERSION_CURRENT;
	super->block_size = sls->block_size;
	super->index_count = 1;
	super->max_saves = sls->save_count;
	super->open_chapter_blocks = sls->open_chapter_blocks;
	super->page_map_blocks = sls->page_map_blocks;
	super->volume_offset = 0;
	super->start_offset = 0;
}

static void setup_sub_index(struct index_layout *layout,
			    uint64_t start_block,
			    struct save_layout_sizes *sls)
{
	struct sub_index_layout *sil = &layout->index;
	uint64_t next_block = start_block;
	unsigned int i;

	sil->sub_index = (struct layout_region) {
		.start_block = start_block,
		.block_count = sls->sub_index_blocks,
		.kind = RL_KIND_INDEX,
		.instance = 0,
	};

	sil->volume = (struct layout_region) {
		.start_block = next_block,
		.block_count = sls->volume_blocks,
		.kind = RL_KIND_VOLUME,
		.instance = RL_SOLE_INSTANCE,
	};

	next_block += sls->volume_blocks;

	for (i = 0; i < sls->save_count; ++i) {
		sil->saves[i].index_save = (struct layout_region) {
			.start_block = next_block,
			.block_count = sls->save_blocks,
			.kind = RL_KIND_SAVE,
			.instance = i,
		};

		next_block += sls->save_blocks;
	}

	define_sub_index_nonce(layout);
}

static void initialize_layout(struct index_layout *layout,
			      struct save_layout_sizes *sls)
{
	uint64_t next_block = layout->offset / sls->block_size;

	layout->total_blocks = sls->total_blocks;
	generate_super_block_data(sls, &layout->super);
	layout->header = (struct layout_region) {
		.start_block = next_block++,
		.block_count = 1,
		.kind = RL_KIND_HEADER,
		.instance = RL_SOLE_INSTANCE,
	};

	layout->config = (struct layout_region) {
		.start_block = next_block++,
		.block_count = 1,
		.kind = RL_KIND_CONFIG,
		.instance = RL_SOLE_INSTANCE,
	};

	setup_sub_index(layout, next_block, sls);
	next_block += sls->sub_index_blocks;

	layout->seal = (struct layout_region) {
		.start_block = next_block,
		.block_count = 1,
		.kind = RL_KIND_SEAL,
		.instance = RL_SOLE_INSTANCE,
	};
}

static int __must_check
make_layout_region_table(struct index_layout *layout,
			 struct region_table **table_ptr)
{
	int result;
	unsigned int i;
	/* Regions: header, config, index, volume, saves, seal */
	uint16_t region_count = 5 + layout->super.max_saves;
	uint16_t payload;
	struct region_table *table;
	struct layout_region *lr;

	result = UDS_ALLOCATE_EXTENDED(struct region_table,
				       region_count,
				       struct layout_region,
				       "layout region table",
				       &table);
	if (result != UDS_SUCCESS) {
		return result;
	}

	lr = &table->regions[0];
	*lr++ = layout->header;
	*lr++ = layout->config;
	*lr++ = layout->index.sub_index;
	*lr++ = layout->index.volume;

	for (i = 0; i < layout->super.max_saves; i++) {
		*lr++ = layout->index.saves[i].index_save;
	}

	*lr++ = layout->seal;

	if (is_converted_super_block(&layout->super)) {
		payload = sizeof(struct super_block_data);
	} else {
		payload = (sizeof(struct super_block_data) -
			   sizeof(layout->super.volume_offset) -
			   sizeof(layout->super.start_offset));
	}

	table->header = (struct region_header) {
		.magic = REGION_MAGIC,
		.region_blocks = layout->total_blocks,
		.type = RH_TYPE_SUPER,
		.version = 1,
		.region_count = region_count,
		.payload = payload,
	};

	*table_ptr = table;
	return UDS_SUCCESS;
}

static int __must_check
encode_index_save_data(struct buffer *buffer,
		       struct index_save_data *save_data)
{
	int result;

	result = put_uint64_le_into_buffer(buffer, save_data->timestamp);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, save_data->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint32_le_into_buffer(buffer, save_data->version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return zero_bytes(buffer, sizeof(uint32_t));
}

static int __must_check
encode_index_state_data(struct buffer *buffer,
			struct index_state_data301 *state_data)
{
	int result;

	result = put_uint32_le_into_buffer(buffer,
					   INDEX_STATE_VERSION_301.signature);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint32_le_into_buffer(buffer,
					   INDEX_STATE_VERSION_301.version_id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state_data->newest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state_data->oldest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state_data->last_save);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = zero_bytes(buffer, sizeof(uint32_t));
	if (result != UDS_SUCCESS) {
		return result;
	}

	return zero_bytes(buffer, sizeof(uint32_t));
}

static int __must_check
encode_region_header(struct buffer *buffer, struct region_header *header)
{
	int result;

	result = put_uint64_le_into_buffer(buffer, REGION_MAGIC);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, header->region_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint16_le_into_buffer(buffer, header->type);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint16_le_into_buffer(buffer, header->version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint16_le_into_buffer(buffer, header->region_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return put_uint16_le_into_buffer(buffer, header->payload);
}

static int __must_check
encode_layout_region(struct buffer *buffer, struct layout_region *region)
{
	int result;

	result = put_uint64_le_into_buffer(buffer, region->start_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, region->block_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = zero_bytes(buffer, 4);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint16_le_into_buffer(buffer, region->kind);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return put_uint16_le_into_buffer(buffer, region->instance);
}

static int __must_check encode_super_block_data(struct buffer *buffer,
						struct super_block_data *super)
{
	int result;

	result = put_bytes(buffer, MAGIC_SIZE, &super->magic_label);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_bytes(buffer, NONCE_INFO_SIZE, &super->nonce_info);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, super->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint32_le_into_buffer(buffer, super->version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint32_le_into_buffer(buffer, super->block_size);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint16_le_into_buffer(buffer, super->index_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint16_le_into_buffer(buffer, super->max_saves);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = zero_bytes(buffer, 4);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, super->open_chapter_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, super->page_map_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (is_converted_super_block(super)) {
		result = put_uint64_le_into_buffer(buffer,
						   super->volume_offset);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = put_uint64_le_into_buffer(buffer,
						   super->start_offset);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

static int __must_check
make_index_save_region_table(struct index_save_layout *isl,
			     struct region_table **table_ptr)
{
	int result;
	unsigned int z;
	struct region_table *table;
	struct layout_region *lr;
	uint16_t region_count;
	size_t payload;
	size_t type;

	if (isl->zone_count > 0) {
		/*
		 * Normal save regions: header, page map, volume index zones,
		 * open chapter, and possibly free space.
		 */
		region_count = 3 + isl->zone_count;
		if (isl->free_space.block_count > 0) {
			region_count++;
		}

		payload = sizeof(isl->save_data) + sizeof(isl->state_data);
		type = RH_TYPE_SAVE;
	} else {
		/* Empty save regions: header, page map, free space. */
		region_count = 3;
		payload = sizeof(isl->save_data);
		type = RH_TYPE_UNSAVED;
	}

	result = UDS_ALLOCATE_EXTENDED(struct region_table,
				       region_count,
				       struct layout_region,
				       "layout region table for ISL",
				       &table);
	if (result != UDS_SUCCESS) {
		return result;
	}

	lr = &table->regions[0];
	*lr++ = isl->header;
	*lr++ = isl->index_page_map;
	for (z = 0; z < isl->zone_count; ++z) {
		*lr++ = isl->volume_index_zones[z];
	}

	if (isl->zone_count > 0) {
		*lr++ = isl->open_chapter;
	}

	if (isl->free_space.block_count > 0) {
		*lr++ = isl->free_space;
	}

	table->header = (struct region_header) {
		.magic = REGION_MAGIC,
		.region_blocks = isl->index_save.block_count,
		.type = type,
		.version = 1,
		.region_count = region_count,
		.payload = payload,
	};

	*table_ptr = table;
	return UDS_SUCCESS;
}

static int __must_check
write_index_save_header(struct index_save_layout *isl,
			struct region_table *table,
			struct buffered_writer *writer)
{
	unsigned int i;
	struct buffer *buffer;
	int result;
	size_t table_size = sizeof(struct region_table) +
		table->header.region_count * sizeof(struct layout_region);

	result = make_buffer(table_size + table->header.payload, &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_region_header(buffer, &table->header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	for (i = 0; i < table->header.region_count; i++) {
		result = encode_layout_region(buffer, &table->regions[i]);
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}
	}

	result = encode_index_save_data(buffer, &isl->save_data);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	if (isl->zone_count > 0) {
		result = encode_index_state_data(buffer, &isl->state_data);
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}
	}

	result = write_to_buffered_writer(writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	return flush_buffered_writer(writer);
}

static int write_index_save_layout(struct index_layout *layout,
				   struct index_save_layout *isl)
{
	struct region_table *table;
	struct buffered_writer *writer = NULL;
	int result = make_index_save_region_table(isl, &table);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_region_writer(layout, &isl->header, &writer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return result;
	}

	result = write_index_save_header(isl, table, writer);
	UDS_FREE(table);
	free_buffered_writer(writer);

	return result;
}

static int __must_check write_layout_header(struct index_layout *layout,
					    struct region_table *table,
					    struct buffered_writer *writer)
{
	int result;
	unsigned int i;
	size_t table_size = sizeof(struct region_table) +
		table->header.region_count * sizeof(struct layout_region);
	struct buffer *buffer;

	result = make_buffer(table_size + table->header.payload, &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_region_header(buffer, &table->header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	for (i = 0; i < table->header.region_count; i++) {
		result = encode_layout_region(buffer, &table->regions[i]);
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}
	}

	result = encode_super_block_data(buffer, &layout->super);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	return flush_buffered_writer(writer);
}

static int __must_check save_layout(struct index_layout *layout, off_t offset)
{
	int result;
	struct buffered_writer *writer = NULL;
	struct region_table *table;

	result = make_layout_region_table(layout, &table);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_layout_writer(layout, &layout->header, offset, &writer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return result;
	}

	result = write_layout_header(layout, table, writer);
	UDS_FREE(table);
	free_buffered_writer(writer);

	return result;
}

static int __must_check
write_uds_index_config(struct index_layout *layout,
		       struct configuration *config,
		       off_t offset)
{
	int result;
	struct buffered_writer *writer = NULL;

	result = open_layout_writer(layout, &layout->config, offset, &writer);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "failed to open config region");
	}

	result = write_config_contents(writer, config, layout->super.version);
	if (result != UDS_SUCCESS) {
		free_buffered_writer(writer);
		return uds_log_error_strerror(result,
					      "failed to write config region");
	}
	result = flush_buffered_writer(writer);
	if (result != UDS_SUCCESS) {
		free_buffered_writer(writer);
		return uds_log_error_strerror(result,
					      "cannot flush config writer");
	}
	free_buffered_writer(writer);
	return UDS_SUCCESS;
}

static int create_index_layout(struct index_layout *layout,
			       struct configuration *config)
{
	int result;
	struct save_layout_sizes sizes;

	result = compute_sizes(config, &sizes);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(sizes.save_count,
			      struct index_save_layout,
			      __func__,
			      &layout->index.saves);
	if (result != UDS_SUCCESS) {
		return result;
	}

	initialize_layout(layout, &sizes);

	result = discard_index_state_data(layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = save_layout(layout, 0);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return write_uds_index_config(layout, config, 0);
}

static int create_layout_factory(struct index_layout *layout,
				 const struct configuration *config,
				 bool new_layout)
{
	int result;
	size_t writable_size;
	struct io_factory *factory = NULL;

	result = make_uds_io_factory(config->name, &factory);
	if (result != UDS_SUCCESS) {
		return result;
	}

	writable_size = get_uds_writable_size(factory) & -UDS_BLOCK_SIZE;
	if (writable_size < config->size + config->offset) {
		put_uds_io_factory(factory);
		uds_log_error("index storage (%zu) is smaller than the requested size %zu",
			      writable_size,
			      config->size + config->offset);
		return -ENOSPC;
	}

	layout->factory = factory;
	layout->factory_size =
		(config->size > 0) ? config->size : writable_size;
	layout->offset = config->offset;
	return UDS_SUCCESS;
}

int make_uds_index_layout(struct configuration *config,
			  bool new_layout,
			  struct index_layout **layout_ptr)
{
	int result;
	struct index_layout *layout = NULL;
	struct save_layout_sizes sizes;

	result = compute_sizes(config, &sizes);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(1, struct index_layout, __func__, &layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = create_layout_factory(layout, config, new_layout);
	if (result != UDS_SUCCESS) {
		free_uds_index_layout(layout);
		return result;
	}

	if (layout->factory_size < sizes.total_size) {
		uds_log_error("index storage (%zu) is smaller than the required size %llu",
			      layout->factory_size,
			      (unsigned long long) sizes.total_size);
		free_uds_index_layout(layout);
		return -ENOSPC;
	}

	if (new_layout) {
		result = create_index_layout(layout, config);
	} else {
		result = load_index_layout(layout, config);
	}
	if (result != UDS_SUCCESS) {
		free_uds_index_layout(layout);
		return result;
	}

	*layout_ptr = layout;
	return UDS_SUCCESS;
}

void free_uds_index_layout(struct index_layout *layout)
{
	if (layout == NULL) {
		return;
	}

	UDS_FREE(layout->index.saves);
	if (layout->factory != NULL) {
		put_uds_io_factory(layout->factory);
	}

	UDS_FREE(layout);
}

int replace_index_layout_storage(struct index_layout *layout,
				 const char *name)
{
	return replace_uds_storage(layout->factory, name);
}

/* Obtain a dm_bufio_client for the volume region. */
int open_uds_volume_bufio(struct index_layout *layout,
			  size_t block_size,
			  unsigned int reserved_buffers,
			  struct dm_bufio_client **client_ptr)
{
	off_t offset = (layout->index.volume.start_block +
			layout->super.volume_offset -
			layout->super.start_offset) *
		layout->super.block_size;

	return make_uds_bufio(layout->factory,
			      offset,
			      block_size,
			      reserved_buffers,
			      client_ptr);
}

uint64_t get_uds_volume_nonce(struct index_layout *layout)
{
	return layout->index.nonce;
}

static uint64_t generate_index_save_nonce(uint64_t volume_nonce,
					  struct index_save_layout *isl)
{
	struct save_nonce_data {
		struct index_save_data data;
		uint64_t offset;
	} nonce_data;
	byte buffer[sizeof(nonce_data)];
	size_t offset = 0;

	encode_uint64_le(buffer, &offset, isl->save_data.timestamp);
	encode_uint64_le(buffer, &offset, 0);
	encode_uint32_le(buffer, &offset, isl->save_data.version);
	encode_uint32_le(buffer, &offset, 0U);
	encode_uint64_le(buffer, &offset, isl->index_save.start_block);
	ASSERT_LOG_ONLY(offset == sizeof(nonce_data),
			"%zu bytes encoded of %zu expected",
			offset,
			sizeof(nonce_data));
	return generate_secondary_nonce(volume_nonce, buffer, sizeof(buffer));
}

static uint64_t validate_index_save_layout(struct index_save_layout *isl,
					   uint64_t volume_nonce)
{
	if ((isl->zone_count == 0) || (isl->save_data.timestamp == 0)) {
		return 0;
	}
	if (isl->save_data.nonce !=
	    generate_index_save_nonce(volume_nonce, isl)) {
		return 0;
	}

	return isl->save_data.timestamp;
}

static void select_oldest_index_save_layout(struct sub_index_layout *sil,
					    unsigned int max_saves,
					    struct index_save_layout **isl_ptr)
{
	struct index_save_layout *oldest = NULL;
	uint64_t save_time = 0;
	uint64_t oldest_time = 0;
	struct index_save_layout *isl;

	for (isl = sil->saves; isl < sil->saves + max_saves; ++isl) {
		save_time = validate_index_save_layout(isl, sil->nonce);
		if (oldest == NULL || save_time < oldest_time) {
			oldest = isl;
			oldest_time = save_time;
		}
	}

	*isl_ptr = oldest;
}

static int __must_check
select_latest_index_save_layout(struct sub_index_layout *sil,
				unsigned int max_saves,
				struct index_save_layout **isl_ptr)
{
	struct index_save_layout *latest = NULL;
	uint64_t save_time = 0;
	uint64_t latest_time = 0;
	struct index_save_layout *isl;

	for (isl = sil->saves; isl < sil->saves + max_saves; ++isl) {
		save_time = validate_index_save_layout(isl, sil->nonce);
		if (save_time > latest_time) {
			latest = isl;
			latest_time = save_time;
		}
	}

	if (latest == NULL) {
		uds_log_error("No valid index save found");
		return UDS_INDEX_NOT_SAVED_CLEANLY;
	}

	*isl_ptr = latest;
	return UDS_SUCCESS;
}

static void instantiate_index_save_layout(struct index_save_layout *isl,
					  struct super_block_data *super,
					  uint64_t volume_nonce,
					  unsigned int zone_count)
{
	isl->zone_count = zone_count;
	populate_index_save_layout(isl, super);

	memset(&isl->save_data, 0, sizeof(isl->save_data));
	isl->save_data.timestamp =
		ktime_to_ms(current_time_ns(CLOCK_REALTIME));
	isl->save_data.version = 1;
	isl->save_data.nonce = generate_index_save_nonce(volume_nonce, isl);
}

static int __must_check
invalidate_old_save(struct index_layout *layout, struct index_save_layout *isl)
{
	reset_index_save_layout(isl, layout->super.page_map_blocks);
	return write_index_save_layout(layout, isl);
}

static int setup_uds_index_save_slot(struct index_layout *layout,
				     unsigned int zone_count,
				     struct index_save_layout **isl_ptr)
{
	int result;
	struct sub_index_layout *sil = &layout->index;
	struct index_save_layout *isl;

	select_oldest_index_save_layout(sil, layout->super.max_saves, &isl);

	result = invalidate_old_save(layout, isl);
	if (result != UDS_SUCCESS) {
		return result;
	}

	instantiate_index_save_layout(isl,
				      &layout->super,
				      sil->nonce,
				      zone_count);

	*isl_ptr = isl;
	return UDS_SUCCESS;
}

static int find_latest_uds_index_save_slot(struct index_layout *layout,
					   struct index_save_layout **isl)
{
	return select_latest_index_save_layout(&layout->index,
					       layout->super.max_saves,
					       isl);
}

static void cancel_uds_index_save(struct index_save_layout *isl)
{
	memset(&isl->save_data, 0, sizeof(isl->save_data));
	memset(&isl->state_data, 0, sizeof(isl->state_data));
	isl->zone_count = 0;
}

int load_index_state(struct index_layout *layout, struct uds_index *index)
{
	int result;
	unsigned int zone;
	struct index_save_layout *isl;
	struct buffered_reader *readers[MAX_ZONES];

	result = find_latest_uds_index_save_slot(layout, &isl);
	if (result != UDS_SUCCESS) {
		return result;
	}

	index->newest_virtual_chapter = isl->state_data.newest_chapter;
	index->oldest_virtual_chapter = isl->state_data.oldest_chapter;
	index->last_save = isl->state_data.last_save;

	result = open_region_reader(layout, &isl->open_chapter, &readers[0]);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = load_open_chapters(index, readers[0]);
	free_buffered_reader(readers[0]);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (zone = 0; zone < isl->zone_count; zone++) {
		result = open_region_reader(layout,
					    &isl->volume_index_zones[zone],
					    &readers[zone]);
		if (result != UDS_SUCCESS) {
			for (; zone > 0; zone--) {
				free_buffered_reader(readers[zone - 1]);
			}

			return result;
		}
	}

	result = load_volume_index(index->volume_index,
				   readers,
				   isl->zone_count);
	for (zone = 0; zone < isl->zone_count; zone++) {
		free_buffered_reader(readers[zone]);
	}
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_region_reader(layout, &isl->index_page_map, &readers[0]);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_index_page_map(index->volume->index_page_map,
				     readers[0]);
	free_buffered_reader(readers[0]);

	return result;
}

int save_index_state(struct index_layout *layout, struct uds_index *index)
{
	int result;
	unsigned int zone;
	struct index_save_layout *isl;
	struct buffered_writer *writers[MAX_ZONES];

	result = setup_uds_index_save_slot(layout, index->zone_count, &isl);
	if (result != UDS_SUCCESS) {
		return result;
	}

	isl->state_data	= (struct index_state_data301) {
		.newest_chapter = index->newest_virtual_chapter,
		.oldest_chapter = index->oldest_virtual_chapter,
		.last_save = index->last_save,
	};

	result = open_region_writer(layout, &isl->open_chapter, &writers[0]);
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(isl);
		return result;
	}

	result = save_open_chapters(index, writers[0]);
	free_buffered_writer(writers[0]);
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(isl);
		return result;
	}

	for (zone = 0; zone < index->zone_count; zone++) {
		result = open_region_writer(layout,
					    &isl->volume_index_zones[zone],
					    &writers[zone]);
		if (result != UDS_SUCCESS) {
			for (; zone > 0; zone--) {
				free_buffered_writer(writers[zone - 1]);
			}

			cancel_uds_index_save(isl);
			return result;
		}
	}

	result = save_volume_index(index->volume_index,
				   writers,
				   index->zone_count);
	for (zone = 0; zone < index->zone_count; zone++) {
		free_buffered_writer(writers[zone]);
	}
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(isl);
		return result;
	}

	result = open_region_writer(layout, &isl->index_page_map, &writers[0]);
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(isl);
		return result;
	}

	result = write_index_page_map(index->volume->index_page_map,
				      writers[0]);
	free_buffered_writer(writers[0]);
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(isl);
		return result;
	}

	return write_index_save_layout(layout, isl);
}

int discard_index_state_data(struct index_layout *layout)
{
	unsigned int i;
	int result;
	int saved_result = UDS_SUCCESS;
	struct sub_index_layout *sil = &layout->index;

	for (i = 0; i < layout->super.max_saves; ++i) {
		result = invalidate_old_save(layout, &sil->saves[i]);
		if (result != UDS_SUCCESS) {
			saved_result = result;
		}
	}

	if (saved_result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "%s: cannot destroy all index saves",
					      __func__);
	}

	return UDS_SUCCESS;
}

int discard_open_chapter(struct index_layout *layout)
{
	int result;
	struct index_save_layout *isl;
	struct buffered_writer *writer;

	result = find_latest_uds_index_save_slot(layout, &isl);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_region_writer(layout, &isl->open_chapter, &writer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = write_zeros_to_buffered_writer(writer,
						UDS_BLOCK_SIZE);
	if (result != UDS_SUCCESS) {
		free_buffered_writer(writer);
		return result;
	}

	result = flush_buffered_writer(writer);
	free_buffered_writer(writer);
	return result;
}
