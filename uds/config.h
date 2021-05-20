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
 * $Id: //eng/uds-releases/jasper/src/uds/config.h#4 $
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
  // XXX the checkpointFrequency is not used - it is now a runtime parameter
  unsigned int checkpointFrequency;
  /** The master index mean delta to use */
  unsigned int masterIndexMeanDelta;
  /** Size of a page, used for both record pages and index pages */
  unsigned int bytesPerPage;
  /** Sampling rate for sparse indexing */
  unsigned int sparseSampleRate;
  /** Index Owner's nonce */
  UdsNonce     nonce;
  /** Virtual chapter remapped from physical chapter 0 in order
   * to reduce chaptersPerVolume by one */
  uint64_t remappedChapter;
  /** Offset by which the remapped chapter was moved, one more
   * than the post-remapping physical chapter number to which it
   * was remapped */
  uint64_t chapterOffset;
};

/**
 * Data that are used for configuring a 6.02 index.
 **/
struct udsConfiguration_6_02 {
  /** Smaller (16), Small (64) or large (256) indices */
  unsigned int recordPagesPerChapter;
  /** Total number of chapters per volume */
  unsigned int chaptersPerVolume;
  /** Number of sparse chapters per volume */
  unsigned int sparseChaptersPerVolume;
  /** Size of the page cache, in chapters */
  unsigned int cacheChapters;
  /** Frequency with which to checkpoint */
  // XXX the checkpointFrequency is not used - it is now a runtime
  // parameter
  unsigned int checkpointFrequency;
  /** The volume index mean delta to use */
  unsigned int masterIndexMeanDelta;
  /** Size of a page, used for both record pages and index pages */
  unsigned int bytesPerPage;
  /** Sampling rate for sparse indexing */
  unsigned int sparseSampleRate;
  /** Index Owner's nonce */
  UdsNonce nonce;
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
 * Write the index configuration information to stable storage.  If
 * the superblock version is < 4 write the 6.02 version; otherwise
 * write the 8.02 version, indicating the configuration is for an
 * index that has been reduced by one chapter.
 * 
 * @param writer        A buffered writer.
 * @param config        The index configuration.
 * @param version       The index superblock version
 *
 * @return UDS_SUCCESS or an error code.
 **/
int writeConfigContents(BufferedWriter   *writer,
                        UdsConfiguration  config,
                        uint32_t          version)
  __attribute__((warn_unused_result));

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
bool areUdsConfigurationsEqual(UdsConfiguration a, UdsConfiguration b)
  __attribute__((warn_unused_result));

/**
 * Log a user configuration.
 *
 * @param conf    The configuration
 **/
void logUdsConfiguration(UdsConfiguration conf);

#endif /* CONFIG_H */
