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
 * $Id: //eng/uds-releases/krusty/src/uds/config.h#9 $
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "bufferedReader.h"
#include "bufferedWriter.h"
#include "geometry.h"
#include "uds.h"

enum {
	DEFAULT_VOLUME_INDEX_MEAN_DELTA = 4096,
	DEFAULT_CACHE_CHAPTERS = 7,
	DEFAULT_SPARSE_SAMPLE_RATE = 0
};

/**
 * Data that are used for configuring a new index.
 **/
struct uds_configuration {
	/** Smaller (16), Small (64) or large (256) indices */
	unsigned int record_pages_per_chapter;
	/** Total number of chapters per volume */
	unsigned int chapters_per_volume;
	/** Number of sparse chapters per volume */
	unsigned int sparse_chapters_per_volume;
	/** Size of the page cache, in chapters */
	unsigned int cache_chapters;
	/** Frequency with which to checkpoint */
	// XXX the checkpoint_frequency is not used - it is now a runtime
	// parameter
	unsigned int checkpoint_frequency;
	/** The volume index mean delta to use */
	unsigned int volume_index_mean_delta;
	/** Size of a page, used for both record pages and index pages */
	unsigned int bytes_per_page;
	/** Sampling rate for sparse indexing */
	unsigned int sparse_sample_rate;
	/** Index Owner's nonce */
	uds_nonce_t nonce;
};

struct index_location {
	char *host;
	char *port;
	char *directory;
};

/**
 * A set of configuration parameters for the indexer.
 **/
struct configuration;

/**
 * Construct a new indexer configuration.
 *
 * @param conf        uds_configuration to use
 * @param config_ptr  The new index configuration
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_configuration(const struct uds_configuration *conf,
				    struct configuration **config_ptr);

/**
 * Clean up the configuration struct.
 **/
void free_configuration(struct configuration *config);

/**
 * Read the index configuration from stable storage.
 *
 * @param reader        A buffered reader.
 * @param config        The index configuration to overwrite.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check read_config_contents(struct buffered_reader *reader,
				      struct uds_configuration *config);

/**
 * Write the index configuration information to stable storage.
 *
 * @param writer        A buffered writer.
 * @param config        The index configuration.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check write_config_contents(struct buffered_writer *writer,
				       struct uds_configuration *config);

/**
 * Free the memory used by an index_location.
 *
 * @param loc   index location to free
 **/
void free_index_location(struct index_location *loc);

/**
 * Compare two configurations for equality.
 *
 * @param a  The first configuration to compare
 * @param b  The second configuration to compare
 *
 * @return true iff they are equal
 **/
bool __must_check are_uds_configurations_equal(struct uds_configuration *a,
					       struct uds_configuration *b);

/**
 * Log a user configuration.
 *
 * @param conf    The configuration
 **/
void log_uds_configuration(struct uds_configuration *conf);

#endif /* CONFIG_H */
