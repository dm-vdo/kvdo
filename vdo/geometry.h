/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef GEOMETRY_H
#define GEOMETRY_H 1

#include "compiler.h"
#include "type-defs.h"
#include "uds.h"

struct geometry {
	/* Size of a chapter page, in bytes */
	size_t bytes_per_page;
	/* Number of record pages in a chapter */
	unsigned int record_pages_per_chapter;
	/* Total number of chapters in a volume */
	unsigned int chapters_per_volume;
	/* Number of sparsely-indexed chapters in a volume */
	unsigned int sparse_chapters_per_volume;
	/* Number of bits used to determine delta list numbers */
	unsigned int chapter_delta_list_bits;
	/* Virtual chapter remapped from physical chapter 0 */
	uint64_t remapped_virtual;
	/* New physical chapter where the remapped chapter can be found */
	uint64_t remapped_physical;

	/*
	 * The following properties are derived from the ones above, but they
         * are computed and recorded as fields for convenience.
	 */
	/* Total number of pages in a volume, excluding the header */
	unsigned int pages_per_volume;
	/* Total number of header pages per volume */
	unsigned int header_pages_per_volume;
	/* Total number of bytes in a volume, including the header */
	size_t bytes_per_volume;
	/* Number of pages in a chapter */
	unsigned int pages_per_chapter;
	/* Number of index pages in a chapter index */
	unsigned int index_pages_per_chapter;
	/* The minimum ratio of hash slots to records in an open chapter */
	unsigned int open_chapter_load_ratio;
	/* Number of records that fit on a page */
	unsigned int records_per_page;
	/* Number of records that fit in a chapter */
	unsigned int records_per_chapter;
	/* Number of records that fit in a volume */
	uint64_t records_per_volume;
	/* Number of delta lists per chapter index */
	unsigned int delta_lists_per_chapter;
	/* Mean delta for chapter indexes */
	unsigned int chapter_mean_delta;
	/* Number of bits needed for record page numbers */
	unsigned int chapter_payload_bits;
	/* Number of bits used to compute addresses for chapter delta lists */
	unsigned int chapter_address_bits;
	/* Number of densely-indexed chapters in a volume */
	unsigned int dense_chapters_per_volume;
};

enum {
	/* The number of bytes in a record (name + metadata) */
	BYTES_PER_RECORD = (UDS_CHUNK_NAME_SIZE + UDS_METADATA_SIZE),

	/* The default length of a page in a chapter, in bytes */
	DEFAULT_BYTES_PER_PAGE = 1024 * BYTES_PER_RECORD,

	/* The default maximum number of records per page */
	DEFAULT_RECORDS_PER_PAGE = DEFAULT_BYTES_PER_PAGE / BYTES_PER_RECORD,

	/* The default number of record pages in a chapter */
	DEFAULT_RECORD_PAGES_PER_CHAPTER = 256,

	/* The default number of record pages in a chapter for a small index */
	SMALL_RECORD_PAGES_PER_CHAPTER = 64,

	/* The default number of chapters in a volume */
	DEFAULT_CHAPTERS_PER_VOLUME = 1024,

	/* The default number of sparsely-indexed chapters in a volume */
	DEFAULT_SPARSE_CHAPTERS_PER_VOLUME = 0,

	/* The log2 of the default mean delta */
	DEFAULT_CHAPTER_MEAN_DELTA_BITS = 16,

	/* The log2 of the number of delta lists in a large chapter */
	DEFAULT_CHAPTER_DELTA_LIST_BITS = 12,

	/* The log2 of the number of delta lists in a small chapter */
	SMALL_CHAPTER_DELTA_LIST_BITS = 10,

	/* The default minimum ratio of slots to records in an open chapter */
	DEFAULT_OPEN_CHAPTER_LOAD_RATIO = 2,
};

int __must_check make_geometry(size_t bytes_per_page,
			       unsigned int record_pages_per_chapter,
			       unsigned int chapters_per_volume,
			       unsigned int sparse_chapters_per_volume,
			       uint64_t remapped_virtual,
			       uint64_t remapped_physical,
			       struct geometry **geometry_ptr);

int __must_check copy_geometry(struct geometry *source,
			       struct geometry **geometry_ptr);

void free_geometry(struct geometry *geometry);

unsigned int __must_check
map_to_physical_chapter(const struct geometry *geometry,
			uint64_t virtual_chapter);

/*
 * Check whether this geometry is reduced by a chapter. This will only be true
 * if the volume was converted from a non-lvm volume to an lvm volume.
 */
static INLINE bool __must_check
is_reduced_geometry(const struct geometry *geometry)
{
	return !!(geometry->chapters_per_volume & 1);
}

static INLINE bool __must_check
is_sparse_geometry(const struct geometry *geometry)
{
	return (geometry->sparse_chapters_per_volume > 0);
}

bool __must_check has_sparse_chapters(const struct geometry *geometry,
				      uint64_t oldest_virtual_chapter,
				      uint64_t newest_virtual_chapter);

bool __must_check is_chapter_sparse(const struct geometry *geometry,
				    uint64_t oldest_virtual_chapter,
				    uint64_t newest_virtual_chapter,
				    uint64_t virtual_chapter_number);

unsigned int __must_check chapters_to_expire(const struct geometry *geometry,
					     uint64_t newest_chapter);

#endif /* GEOMETRY_H */
