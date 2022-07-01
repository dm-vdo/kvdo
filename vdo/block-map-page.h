/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef BLOCK_MAP_PAGE_H
#define BLOCK_MAP_PAGE_H

#include "numeric.h"

#include "block-map-entry.h"
#include "header.h"
#include "types.h"

struct block_map_page_header {
	__le64 nonce;
	__le64 pbn;

	/** May be non-zero on disk */
	byte unused_long_word[8];

	/* Whether this page has been written twice to disk */
	bool initialized;

	/* Always zero on disk */
	byte unused_byte1;

	/* May be non-zero on disk */
	byte unused_byte2;
	byte unused_byte3;
} __packed;

struct block_map_page {
	struct packed_version_number version;
	struct block_map_page_header header;
	struct block_map_entry entries[];
} __packed;

enum block_map_page_validity {
	VDO_BLOCK_MAP_PAGE_VALID,
	VDO_BLOCK_MAP_PAGE_INVALID,
	/* Valid page found in the wrong location on disk */
	VDO_BLOCK_MAP_PAGE_BAD,
};

static inline bool __must_check
vdo_is_block_map_page_initialized(const struct block_map_page *page)
{
	return page->header.initialized;
}

static inline bool
vdo_mark_block_map_page_initialized(struct block_map_page *page,
				    bool initialized)
{
	if (initialized == page->header.initialized) {
		return false;
	}

	page->header.initialized = initialized;
	return true;
}

static inline physical_block_number_t __must_check
vdo_get_block_map_page_pbn(const struct block_map_page *page)
{
	return __le64_to_cpu(page->header.pbn);
}

struct block_map_page *vdo_format_block_map_page(void *buffer,
						 nonce_t nonce,
						 physical_block_number_t pbn,
						 bool initialized);

enum block_map_page_validity __must_check
vdo_validate_block_map_page(struct block_map_page *page,
			     nonce_t nonce,
			     physical_block_number_t pbn);

#endif /* BLOCK_MAP_PAGE_H */
