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
 * $Id: //eng/uds-releases/krusty/src/uds/config.h#2 $
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "bufferedReader.h"
#include "bufferedWriter.h"
#include "geometry.h"
#include "uds.h"

enum {
  DEFAULT_MASTER_INDEX_MEAN_DELTA   = 4096,
  DEFAULT_CACHE_CHAPTERS            = 7,
  DEFAULT_SPARSE_SAMPLE_RATE        = 0
};

/**
 * Data that are used for configuring a new index.
 **/
struct uds_configuration {
  /** Smaller (16), Small (64) or large (256) indices */
  unsigned int recordPagesPerChapter;
  /** Total number of chapters per volume */
  unsigned int chaptersPerVolume;
  /** Number of sparse chapters per volume */
  unsigned int sparseChaptersPerVolume;
  /** Size of the page cache, in chapters */
  unsigned int cacheChapters;
  /** Frequency with which to checkpoint */
  // XXX the checkpointFrequency is not used - it is now a runtime parameter
  unsigned int checkpointFrequency;
  /** The master index mean delta to use */
  unsigned int masterIndexMeanDelta;
  /** Size of a page, used for both record pages and index pages */
  unsigned int bytesPerPage;
  /** Sampling rate for sparse indexing */
  unsigned int sparseSampleRate;
  /** Index Owner's nonce */
  uds_nonce_t  nonce;
};

/**
 * Data that are used for a 6.01 index.
 **/
struct uds_configuration_6_01 {
  /** Smaller (16), Small (64) or large (256) indices */
  unsigned int recordPagesPerChapter;
  /** Total number of chapters per volume */
  unsigned int chaptersPerVolume;
  /** Number of sparse chapters per volume */
  unsigned int sparseChaptersPerVolume;
  /** Size of the page cache, in chapters */
  unsigned int cacheChapters;
  /** Frequency with which to checkpoint */
  unsigned int checkpointFrequency;
  /** The master index mean delta to use */
  unsigned int masterIndexMeanDelta;
  /** Size of a page, used for both record pages and index pages */
  unsigned int bytesPerPage;
  /** Sampling rate for sparse indexing */
  unsigned int sparseSampleRate;
};

typedef struct indexLocation {
  char *host;
  char *port;
  char *directory;
} IndexLocation;

/**
 * A set of configuration parameters for the indexer.
 **/
typedef struct configuration Configuration;

/**
 * Construct a new indexer configuration.
 *
 * @param conf       uds_configuration to use
 * @param configPtr  The new index configuration
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
makeConfiguration(const struct uds_configuration *conf,
                  Configuration **configPtr);

/**
 * Clean up the configuration struct.
 **/
void freeConfiguration(Configuration *config);

/**
 * Read the index configuration from stable storage.
 *
 * @param reader        A buffered reader.
 * @param config        The index configuration to overwrite.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
readConfigContents(BufferedReader *reader, struct uds_configuration *config);

/**
 * Write the index configuration information to stable storage.
 *
 * @param writer        A buffered writer.
 * @param config        The index configuration.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
writeConfigContents(BufferedWriter *writer, struct uds_configuration *config);

/**
 * Free the memory used by an IndexLocation.
 *
 * @param loc   index location to free
 **/
void freeIndexLocation(IndexLocation *loc);

/**
 * Compare two configurations for equality.
 *
 * @param a  The first configuration to compare
 * @param b  The second configuration to compare
 *
 * @return true iff they are equal
 **/
bool __must_check
areUdsConfigurationsEqual(struct uds_configuration *a,
                          struct uds_configuration *b);

/**
 * Log a user configuration.
 *
 * @param conf    The configuration
 **/
void logUdsConfiguration(struct uds_configuration *conf);

#endif /* CONFIG_H */
