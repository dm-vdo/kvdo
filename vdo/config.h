/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "buffered-reader.h"
#include "buffered-writer.h"
#include "geometry.h"
#include "uds.h"

enum {
	DEFAULT_VOLUME_INDEX_MEAN_DELTA = 4096,
	DEFAULT_CACHE_CHAPTERS = 7,
	DEFAULT_SPARSE_SAMPLE_RATE = 32,
	MAX_ZONES = 16,
};

/* A set of configuration parameters for the indexer. */
struct configuration {
	/* String describing the storage device */
	const char *name;

	/* The maximum allowable size of the index */
	size_t size;

	/* The offset where the index should start */
	off_t offset;

	/* Parameters for the volume */

	/* The volume layout */
	struct geometry *geometry;

	/* Index owner's nonce */
	uint64_t nonce;

	/* The number of threads used to process index requests */
	unsigned int zone_count;

	/* The number of threads used to read volume pages */
	unsigned int read_threads;

	/* Size of the page cache and sparse chapter index cache in chapters */
	unsigned int cache_chapters;

	/* Parameters for the volume index */

	/* The mean delta for the volume index */
	unsigned int volume_index_mean_delta;

	/* Sampling rate for sparse indexing */
	unsigned int sparse_sample_rate;
};

/* On-disk structure of data for a version 8.02 index. */
struct uds_configuration_8_02 {
	/* Smaller (16), Small (64) or large (256) indices */
	unsigned int record_pages_per_chapter;
	/* Total number of chapters per volume */
	unsigned int chapters_per_volume;
	/* Number of sparse chapters per volume */
	unsigned int sparse_chapters_per_volume;
	/* Size of the page cache, in chapters */
	unsigned int cache_chapters;
	/* Unused field */
	unsigned int unused;
	/* The volume index mean delta to use */
	unsigned int volume_index_mean_delta;
	/* Size of a page, used for both record pages and index pages */
	unsigned int bytes_per_page;
	/* Sampling rate for sparse indexing */
	unsigned int sparse_sample_rate;
	/* Index owner's nonce */
	uint64_t nonce;
	/* Virtual chapter remapped from physical chapter 0 */
	uint64_t remapped_virtual;
	/* New physical chapter which remapped chapter was moved to */
	uint64_t remapped_physical;
};

/* On-disk structure of data for a version 6.02 index. */
struct uds_configuration_6_02 {
	/* Smaller (16), Small (64) or large (256) indices */
	unsigned int record_pages_per_chapter;
	/* Total number of chapters per volume */
	unsigned int chapters_per_volume;
	/* Number of sparse chapters per volume */
	unsigned int sparse_chapters_per_volume;
	/* Size of the page cache, in chapters */
	unsigned int cache_chapters;
	/* Unused field */
	unsigned int unused;
	/* The volume index mean delta to use */
	unsigned int volume_index_mean_delta;
	/* Size of a page, used for both record pages and index pages */
	unsigned int bytes_per_page;
	/* Sampling rate for sparse indexing */
	unsigned int sparse_sample_rate;
	/* Index owner's nonce */
	uint64_t nonce;
};

int __must_check make_configuration(const struct uds_parameters *params,
				    struct configuration **config_ptr);

void free_configuration(struct configuration *config);

int __must_check validate_config_contents(struct buffered_reader *reader,
					  struct configuration *config);

int __must_check write_config_contents(struct buffered_writer *writer,
				       struct configuration *config,
				       uint32_t version);

void log_uds_configuration(struct configuration *config);

#endif /* CONFIG_H */
