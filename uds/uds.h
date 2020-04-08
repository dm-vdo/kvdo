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
 * $Id: //eng/uds-releases/krusty/src/uds/uds.h#3 $
 */

/**
 * @mainpage UDS API Reference
 * <center>Copyright (c) 2020 Red Hat, Inc.</center>
 **/

/**
 * @file
 * @brief General UDS definitions
 **/
#ifndef UDS_H
#define UDS_H

#include "uds-platform.h"

#ifdef UDS_DISABLE_ATTR_WARN_UNUSED_RESULT
#define UDS_ATTR_WARN_UNUSED_RESULT
#else
#define UDS_ATTR_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#endif

/**
 * Valid request types as described in callbacks.
 **/
typedef enum {
  /**
   * Callback type for operations that post mappings to the UDS
   * index.  When the chunk-hash being added already exists, the
   * existing metadata is not overwritten. Regardless, the
   * recency of the chunk is updated.
   **/
  UDS_POST,

  /**
   * Callback type for operations that update mappings in the UDS
   * index. If the indicated entry does not have any mapping in the
   * index, one is created. In either case, the recency of
   * the chunk is updated.
   **/
  UDS_UPDATE,

  /**
   * Callback type for operations that delete mappings from the
   * UDS index. */
  UDS_DELETE,

  /**
   * Callback type for operations that query mappings in the UDS
   * index. When a mapping is found, the recency of the mapping
   * is updated unless it's the no-update call.
   **/
  UDS_QUERY
} UdsCallbackType;

/**
 * Valid types for opening an index.
 **/
typedef enum {
  /**
   * Load an existing index.  If the index was not saved cleanly, try to
   * recover and rebuild the index.
   **/
  UDS_LOAD = 0,

  /**
   * Create a new index.
   **/
  UDS_CREATE = 1,

  /**
   * Load an existing index, but only if it was cleanly saved.
   **/
  UDS_NO_REBUILD = 2,
} UdsOpenIndexType;

/** General UDS constants. */
enum {
  /** The chunk name size in bytes (128 bits = 16 bytes). */
  UDS_CHUNK_NAME_SIZE   = 16,
  /** The maximum metadata size in bytes. */
  UDS_MAX_METADATA_SIZE = 16,
};

/**
 *  Type representing memory configuration which is either a positive
 *  integer number of gigabytes or one of the three special constants
 *  for configurations which are smaller than 1 gigabyte.
 **/
typedef unsigned int UdsMemoryConfigSize;

extern const UdsMemoryConfigSize UDS_MEMORY_CONFIG_256MB;
extern const UdsMemoryConfigSize UDS_MEMORY_CONFIG_512MB;
extern const UdsMemoryConfigSize UDS_MEMORY_CONFIG_768MB;

/**
 *  The maximum configurable amount of memory.
 **/
extern const UdsMemoryConfigSize UDS_MEMORY_CONFIG_MAX;

/** The name (hash) of a chunk. */
typedef struct udsChunkName {
  /** The name (hash) of a chunk. */
  unsigned char name[UDS_CHUNK_NAME_SIZE];
} UdsChunkName;

/**
 * An active index session.
 **/
struct uds_index_session;

/**
 * The data used to configure a new index.
 **/
typedef struct udsConfiguration *UdsConfiguration;
typedef uint64_t UdsNonce;

/**
 * The data used to configure a new index session.
 **/
struct uds_parameters {
  // Tne number of threads used to process index requests.
  int zone_count;
  // The number of threads used to read volume pages.
  int read_threads;
  // The number of chapters to write between checkpoints.
  int checkpoint_frequency;
};
#define UDS_PARAMETERS_INITIALIZER {		\
		.zone_count = 0,		\
		.read_threads = 2,		\
		.checkpoint_frequency = 0,	\
	}

/**
 * Index statistics
 *
 * These statistics capture the current index characteristics,
 * including resource usage.
 **/
typedef struct udsIndexStats {
  /** The total number of chunk names stored in the index */
  uint64_t entriesIndexed;
  /** An estimate of the index's memory usage */
  uint64_t memoryUsed;
  /** The number of collisions recorded in the master index */
  uint64_t collisions;
  /** The number of entries discarded from the index since index startup */
  uint64_t entriesDiscarded;
  /** The number of checkpoints done this session */
  uint64_t checkpoints;
} UdsIndexStats;

/**
 * Context statistics
 *
 * These statistics capture a library context's characteristics either since
 * it was initialized or since its statistics were last reset, whichever
 * is more recent.
 **/
typedef struct udsContextStats {
  /** The time at which context statistics were last fetched */
  int64_t   currentTime;
  /**
   * The number of post calls since context statistics were last reset that
   * found an existing entry
   **/
  uint64_t postsFound;
  /**
   * The number of post calls since context statistics were last reset that
   * added an entry
   **/
  uint64_t postsNotFound;
  /**
   * The number of post calls since context statistics were last reset that
   * found an existing entry is current enough to only exist in memory and not
   * have been commited to disk yet.
   **/
  uint64_t inMemoryPostsFound;
  /**
   * The number of post calls since context statistics were last reset that
   * found an existing entry in the dense portion of the index.
   **/
  uint64_t densePostsFound;
  /**
   * The number of post calls since context statistics were last reset that
   * found an existing entry in the sparse portion of the index (if one
   * exists).
   **/
  uint64_t sparsePostsFound;
  /**
   * The number of update calls since context statistics were last reset that
   * updated an existing entry
   **/
  uint64_t updatesFound;
  /**
   * The number of update calls since context statistics were last reset that
   * added a new entry
   **/
  uint64_t updatesNotFound;
  /**
   * The number of delete requests since context statistics were last reset
   * that deleted an existing entry
   **/
  uint64_t deletionsFound;
  /**
   * The number of delete requests since context statistics were last reset
   * that did nothing.
   **/
  uint64_t deletionsNotFound;
  /**
   * The number of query calls since context statistics were last reset that
   * found existing entry
   **/
  uint64_t queriesFound;
  /**
   * The number of query calls since context statistics were last reset that
   * did not find an entry
   **/
  uint64_t queriesNotFound;
  /**
   * The total number of library requests (the sum of posts, updates,
   * deletions, and queries) since context
   * statistics were last reset
   **/
  uint64_t requests;
} UdsContextStats;

/**
 * Initializes an index configuration.
 *
 * @param [out] conf          The new configuration
 * @param [in] memGB          The maximum memory allocation, in GB
 *
 * @return                    Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsInitializeConfiguration(UdsConfiguration    *conf,
                               UdsMemoryConfigSize  memGB);

/**
 * Sets or clears an index configuration's sparse indexing settings.
 *
 * @param [in,out] conf       The configuration to change
 * @param [in] sparse         If <code>true</code>, request a sparse
 *                            index; if <code>false</code>, request
 *                            a default index.
 *
 **/
void udsConfigurationSetSparse(UdsConfiguration conf, bool sparse);

/**
 * Tests whether an index configuration specifies sparse indexing.
 *
 * @param [in] conf           The configuration to check
 *
 * @return                    Returns <code>true</code> if the configuration
 *                            is sparse, or <code>false</code> if not
 **/
UDS_ATTR_WARN_UNUSED_RESULT
bool udsConfigurationGetSparse(UdsConfiguration conf);

/**
 * Sets an index configuration's nonce.
 *
 * @param [in,out] conf  The configuration to change
 * @param [in] nonce    The 64 bit nonce.
 *
 **/
void udsConfigurationSetNonce(UdsConfiguration conf, UdsNonce nonce);

/**
 * Gets an index configuration's nonce.
 *
 * @param [in] conf  The configuration to check
 *
 * @return  The 64 bit nonce.
 **/
UDS_ATTR_WARN_UNUSED_RESULT
UdsNonce udsConfigurationGetNonce(UdsConfiguration conf);

/**
 * Fetches a configuration's maximum memory allocation.
 *
 * @param [in] conf  The configuration to check
 *
 * @return      The amount of memory allocated, in GB
 **/
UDS_ATTR_WARN_UNUSED_RESULT
UdsMemoryConfigSize udsConfigurationGetMemory(UdsConfiguration conf);

/**
 * Fetches a configuration's chapters per volume value.
 *
 * @param [in] conf  The configuration to check
 *
 * @return      The number of chapters per volume
 **/
UDS_ATTR_WARN_UNUSED_RESULT
unsigned int udsConfigurationGetChaptersPerVolume(UdsConfiguration conf);

/**
 * Frees memory used by a configuration.
 *
 * @param [in,out] conf The configuration for which memory is being freed
 **/
void udsFreeConfiguration(UdsConfiguration conf);

/**
 * Compute the size required to store the index on persistent storage.  This
 * size is valid for any index stored in a single file or on a single block
 * device.  This size should be used when configuring a block device on which
 * to store an index.
 *
 * @param [in]  config          A UdsConfiguration for an index.
 * @param [in]  numCheckpoints  The maximum number of checkpoints.
 * @param [out] indexSize       The number of bytes required to store
 *                              the index.
 *
 * @return UDS_SUCCESS or an error code.
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int uds_compute_index_size(const UdsConfiguration  config,
                           unsigned int            numCheckpoints,
                           uint64_t               *indexSize);

/**
 * Opens an index session.
 *
 * Creates a session for an index. #udsOpenIndex must be called before
 * the index can be used.
 *
 * Destroy the session with #udsDestroyIndexSession.
 *
 * @param [out] session  A pointer to the new session
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsCreateIndexSession(struct uds_index_session **session);

/**
 * Fetches the UDS library version.
 *
 * @return       The library version
 **/
UDS_ATTR_WARN_UNUSED_RESULT
const char *udsGetVersion(void);

/**
 * The name argument to #udsOpenIndex is a text string that names the index.
 * The name should have the form "path", where path is the name of the block
 * device.  The path should not contain white space.  The names can optionally
 * contain size and/or offset options which give the number of bytes in the
 * index and the byte offset to the start of the index.  For example, the name
 * "/dev/sda8 offset=409600 size=2048000000" is an index that is stored in
 * 2040000000 bytes of /dev/sda8 starting at byte 409600.
 **/

/**
 * Opens an index with an existing session.  This operation will fail if the
 * index session is suspended, or if there is already an open index.
 *
 * The index should be closed with #udsCloseIndex.
 *
 * @param openType  The type of open, which is one of #UDS_LOAD, #UDS_CREATE,
 *                  or #UDS_NO_REBUILD.
 * @param name      The name of the index
 * @param params    The index session parameters.  If NULL, the default
 *                       session parameters will be used.
 * @param conf      The index configuration
 * @param session   The index session
 *
 * @return          Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsOpenIndex(UdsOpenIndexType             openType,
                 const char                  *name,
                 const struct uds_parameters *params,
                 UdsConfiguration             conf,
                 struct uds_index_session    *session);

/**
 * Waits until all callbacks for index operations are complete, and prevents
 * new index operations from starting. Index operations will return
 * UDS_SUSPENDED until #udsResumeIndexSession is called. Optionally saves all
 * index data before returning.
 *
 * @param session  The session to suspend
 * @param save     Whether to save index data
 *
 * @return  Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsSuspendIndexSession(struct uds_index_session *session, bool save);

/**
 * Allows new index operations for an index, whether it was suspended or not.
 *
 * @param session  The session to resume
 *
 * @return  Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsResumeIndexSession(struct uds_index_session *session);

/**
 * Waits until all callbacks for index operations are complete.
 *
 * @param [in] session  The session to flush
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsFlushIndexSession(struct uds_index_session *session);

/**
 * Closes an index.  This operation will fail if the index session is
 * suspended.
 *
 * Saves changes to the index so that #udsOpenIndex can re-open it.
 *
 * @param [in] session  The session containing the index to close
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsCloseIndex(struct uds_index_session *session);

/**
 * Destroys an index session.
 *
 * Saves changes to the index and closes the index if one is open.
 * Use #udsDestroyIndexSession for index sessions created by
 * #udsCreateIndexSession.
 *
 * @param [in] session  The session to destroy
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
int udsDestroyIndexSession(struct uds_index_session *session);

/**
 * Returns the configuration for the given index session.
 *
 * @param [in]  session The session
 * @param [out] conf    The index configuration
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsGetIndexConfiguration(struct uds_index_session *session,
                             UdsConfiguration         *conf);

/**
 * Fetches index statistics for the given index session.
 *
 * @param [in]  session The session
 * @param [out] stats   The index statistics structure to fill
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsGetIndexStats(struct uds_index_session *session, UdsIndexStats *stats);

/**
 * Fetches index session statistics for the given index session.
 *
 * @param [in]  session  The session
 * @param [out] stats    The context statistics structure to fill
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsGetIndexSessionStats(struct uds_index_session *session,
                            UdsContextStats          *stats);

/**
 * Convert an error code to a string.
 *
 * @param errnum       The error code
 * @param buf          The buffer to hold the error string
 * @param buflen       The length of the buffer
 *
 * @return A pointer to buf
 **/
UDS_ATTR_WARN_UNUSED_RESULT
const char *udsStringError(int errnum, char *buf, size_t buflen);

/**
 * Suggested buffer size for udsStringError.
 **/
enum {
  UDS_STRING_ERROR_BUFSIZE = 128
};

#endif /* UDS_H */
