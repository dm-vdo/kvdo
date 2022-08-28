/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef OPENCHAPTER_H
#define OPENCHAPTER_H 1

#include "chapter-index.h"
#include "common.h"
#include "geometry.h"
#include "index.h"
#include "volume.h"

enum {
	OPEN_CHAPTER_RECORD_NUMBER_BITS = 23,
	OPEN_CHAPTER_MAX_RECORD_NUMBER =
	       (1 << OPEN_CHAPTER_RECORD_NUMBER_BITS) - 1
};

struct open_chapter_zone_slot {
	/* If non-zero, the record number addressed by this hash slot */
	unsigned int record_number : OPEN_CHAPTER_RECORD_NUMBER_BITS;
	/* If true, the record at the index of this hash slot was deleted */
	bool record_deleted : 1;
} __packed;

struct open_chapter_zone {
	/* The maximum number of records that can be stored */
	unsigned int capacity;
	/* The number of records stored */
	unsigned int size;
	/* The number of deleted records */
	unsigned int deleted;
	/* Array of chunk records, 1-based */
	struct uds_chunk_record *records;
	/* The number of slots in the hash table */
	unsigned int slot_count;
	/* The hash table slots, referencing virtual record numbers */
	struct open_chapter_zone_slot slots[];
};

int __must_check
make_open_chapter(const struct geometry *geometry,
		  unsigned int zone_count,
		  struct open_chapter_zone **open_chapter_ptr);

size_t __must_check
open_chapter_size(const struct open_chapter_zone *open_chapter);

void reset_open_chapter(struct open_chapter_zone *open_chapter);

void search_open_chapter(struct open_chapter_zone *open_chapter,
			 const struct uds_chunk_name *name,
			 struct uds_chunk_data *metadata,
			 bool *found);

int __must_check put_open_chapter(struct open_chapter_zone *open_chapter,
				  const struct uds_chunk_name *name,
				  const struct uds_chunk_data *metadata,
				  unsigned int *remaining);

void remove_from_open_chapter(struct open_chapter_zone *open_chapter,
			      const struct uds_chunk_name *name);

void free_open_chapter(struct open_chapter_zone *open_chapter);

int __must_check close_open_chapter(struct open_chapter_zone **chapter_zones,
				    unsigned int zone_count,
				    struct volume *volume,
				    struct open_chapter_index *chapter_index,
				    struct uds_chunk_record *collated_records,
				    uint64_t virtual_chapter_number);

int __must_check save_open_chapters(struct uds_index *index,
				    struct buffered_writer *writer);

int __must_check load_open_chapters(struct uds_index *index,
				    struct buffered_reader *reader);

uint64_t compute_saved_open_chapter_size(struct geometry *geometry);

#endif /* OPENCHAPTER_H */
