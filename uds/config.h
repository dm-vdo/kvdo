/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/src/uds/config.h#2 $
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
struct udsConfiguration {
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

/**
 * Data that are used for 4.13 index
 **/
struct udsConfiguration4_13 {
  /** Number of sub indices */
  unsigned int subIndexCount;
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

/**
 * Data that are used for 4.12 index
 **/
struct udsConfiguration4_12 {
  /** Number of sub indices */
  unsigned int subIndexCount;
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
  /** Bits to use for delta calculation in the master index */
  unsigned int masterIndexAddressBits;
  /** Sampling rate for sparse indexing */
  unsigned int sparseSampleRate;
};

/**
 * Configuration for 4.11 index
 **/
struct udsConfiguration4_11 {
  /** Number of sub indices */
  unsigned int subIndexCount;
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
  /** Bits to use for delta list selection in the master index [obsolete] */
  unsigned int masterIndexDeltaBits;
  /** Bits to use for delta calculation in the master index */
  unsigned int masterIndexAddressBits;
  /** Sampling rate for sparse indexing */
  unsigned int sparseSampleRate;
};

/**
 * Data that are used for configuring a 4.0 index.
 **/
struct udsConfiguration4_01 {
  /** Number of sub indices */
  unsigned int subIndexCount;
  /** Smaller (64MB), Small (256MB) or large (1GB) indices */
  unsigned int indexSize;
  /** Total number of chapters per volume */
  unsigned int chaptersPerVolume;
  /** Number of sparse chapters per volume */
  unsigned int sparseChaptersPerVolume;
  /** Size of the page cache, in chapters */
  unsigned int cacheChapters;
  /** Frequency with which to checkpoint */
  unsigned int checkpointFrequency;
  /** The master index version to use */
  unsigned int masterIndexVersion;
  /** Bits to use for delta list selection in the master index */
  unsigned int masterIndexDeltaBits;
  /** Bits to use for delta calculation in the master index */
  unsigned int masterIndexAddressBits;
  /** Sampling rate for sparse indexing */
  unsigned int sparseSampleRate;
};

typedef struct indexLocation {
  char *host;
  char *port;
  char *directory;
} IndexLocation;

struct udsGridConfig {
  unsigned int numLocations;
  unsigned int maxLocations;
  IndexLocation *locations;
};

/**
 * A set of configuration parameters for the indexer.
 **/
typedef struct configuration Configuration;

/**
 * Construct a new indexer configuration.
 *
 * @param conf       UdsConfiguration to use
 * @param configPtr  The new index configuration
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeConfiguration(UdsConfiguration   conf,
                      Configuration    **configPtr)
  __attribute__((warn_unused_result));

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
int readConfigContents(BufferedReader   *reader,
                       UdsConfiguration  config)
  __attribute__((warn_unused_result));

/**
 * Write the index configuration information to stable storage.
 *
 * @param writer        A buffered writer.
 * @param config        The index configuration.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int writeConfigContents(BufferedWriter   *writer,
                        UdsConfiguration  config)
  __attribute__((warn_unused_result));

/**
 * Add a grid server or local directory to a grid config.
 *
 * @param gridConfig  The configuration to which to add the server
 * @param host        The server host
 * @param port        The server port
 * @param directory   The index directory.
 *
 * @return UDS_SUCCESS or an error code
 **/
int addGridServer(UdsGridConfig  gridConfig,
                  const char    *host,
                  const char    *port,
                  const char    *directory)
  __attribute__((warn_unused_result));

/**
 * Free the memory used by an IndexLocation.
 *
 * @param loc   index location to free
 **/
void freeIndexLocation(IndexLocation *loc);

/**
 * Print an index location to a string.
 *
 * @param indexLocation  The index location
 * @param output         The output string.
 *
 * @return UDS_SUCCESS or an error code
 **/
int printIndexLocation(const IndexLocation  *indexLocation,
                       char                **output)
  __attribute__((warn_unused_result));

/**
 * Log the grid configuration
 *
 * @param gridConfig  The grid configuration
 * @param message     The message to log with the configuration
 *
 * @return UDS_SUCCESS or an error code
 **/
void logGridConfig(UdsGridConfig gridConfig, const char *message);

/**
 * Compare two configurations for equality.
 *
 * @param a  The first configuration to compare
 * @param b  The second configuration to compare
 *
 * @return true iff they are equal
 **/
bool areUdsConfigurationsEqual(UdsConfiguration a, UdsConfiguration b)
  __attribute__((warn_unused_result));

/**
 * Print a user configuration to a string.
 *
 * @param conf    The configuration
 * @param indent  The indentation to use before the data
 * @param output  The output string.
 *
 * @return UDS_SUCCESS or an error code
 **/
int printUdsConfiguration(UdsConfiguration conf, int indent, char **output)
  __attribute__((warn_unused_result));

#endif /* CONFIG_H */
