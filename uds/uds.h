/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/gloria/src/public/uds.h#1 $
 */

/**
 * @mainpage UDS API Reference
 * <center>Copyright (c) 2018 Red Hat, Inc.</center>
 **/

/**
 * @file
 * @brief General UDS definitions
 **/
#ifndef UDS_H
#define UDS_H

#ifdef __cplusplus
extern "C" {
#endif

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

/** General UDS constants. */
/** The chunk name size in bytes (128 bits = 16 bytes). */
#define UDS_CHUNK_NAME_SIZE 16
enum {
  /** The maximum metadata size in bytes. */
  UDS_MAX_METADATA_SIZE = 16
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
 * An active index session, either local or grid.
 **/
typedef struct udsIndexSession {
  /** The session ID. */
  unsigned int id;
} UdsIndexSession;

/**
 * The data used to configure a new index.
 **/
typedef struct udsConfiguration *UdsConfiguration;
typedef uint64_t UdsNonce;

/**
 * Index statistics
 *
 * These statistics capture the current index characteristics,
 * including resource usage.
 **/
typedef struct udsIndexStats {
  /** The total number of chunk names stored in the index */
  uint64_t      entriesIndexed;
  /** An estimate of the index's memory usage */
  uint64_t      memoryUsed;
  /** An estimate of the index's size on disk */
  uint64_t      diskUsed;
  /** The number of collisions recorded in the master index */
  uint64_t      collisions;
  /** The number of entries discarded from the index since index startup */
  uint64_t      entriesDiscarded;
  /** The number of checkpoints done this session */
  uint64_t      checkpoints;
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
  time_t   currentTime;
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
 * Sets an index configuration's checkpoint frequency.
 *
 * @param [in,out] conf              The configuration to change
 * @param [in] checkpointFrequency   The number of chapters to write between
 *                                   checkpoints
 *
 * @return                           Either #UDS_SUCCESS or an error code.
 *
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsConfigurationSetCheckpointFrequency(UdsConfiguration conf,
                                           unsigned int checkpointFrequency);

/**
 * Fetches a configuration's checkpoint frequency.
 *
 * @param [in] conf                  The configuration to check
 *
 * @return The checkpoint frequency (the number of chapters written between
 *         checkpoints)
 **/
UDS_ATTR_WARN_UNUSED_RESULT
unsigned int udsConfigurationGetCheckpointFrequency(UdsConfiguration conf);

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
int udsComputeIndexSize(const UdsConfiguration  config,
                        unsigned int            numCheckpoints,
                        uint64_t               *indexSize);

/**
 * Fetches the UDS library version.
 *
 * @return       The library version
 **/
UDS_ATTR_WARN_UNUSED_RESULT
const char *udsGetVersion(void);

/**
 * The name argument to #udsCreateLocalIndex, #udsLoadLocalIndex or
 * #udsRebuildLocalIndex is a text string that names the index.
 *
 * For an index stored in a regular file, the name should have the form
 * "file=path", where path is the name of the directory containing the
 * index.  The path should not contain white space.
 *
 * For an index stored on a block device and accessed through the Linux page
 * cache, the name should have the form "file=path", where path is the name of
 * the block device.  The path should not contain white space.
 *
 * For an index stored on a block device and accessed through the block device
 * interfaces, the name should have the form "dev=path", where path is the name
 * of the block device.  The path should not contain white space.
 *
 * Any of these names can optionally contain size and/or offset options which
 * give the number of bytes in the index and the byte offset to the start of
 * the index.  For example, the name "file=/dev/sda8 offset=409600
 * size=2048000000" is an index that is stored in 2040000000 bytes of /dev/sda8
 * starting at byte 409600.
 **/

/**
 * Initializes a new local index and creates a session for it.
 *
 * The Application Software can use #UdsIndexSession to open any number of
 * block contexts.
 *
 * Close the local index with #udsCloseIndexSession after all of its contexts
 * have been closed.
 *
 * @param [in] name      The name of the index
 * @param [in] conf      The index configuration
 * @param [out] session  The name of the new local index session
 *
 * @return               Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsCreateLocalIndex(const char       *name,
                        UdsConfiguration  conf,
                        UdsIndexSession  *session);

/**
 * Loads a saved index and creates a session for it.
 *
 * This function can only load a cleanly closed local index.  If the index was
 * not cleanly closed, use #udsRebuildLocalIndex.
 *
 * The Application Software can use #UdsIndexSession to open any number of
 * block contexts.
 *
 * Close the local index with #udsCloseIndexSession after all of its contexts
 * have been closed.
 *
 * @param [in] name      The name of the index
 * @param [out] session  The name of the new local index session
 *
 * @return               Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsLoadLocalIndex(const char *name, UdsIndexSession *session);

/**
 * Loads a saved index, rebuilds it if necessary, and creates a session for it.
 *
 * This function can only load a local index.  If the index was not cleanly
 * closed, then it is rebuilt from the most recent consistent checkpoint.
 *
 * The application can use #UdsIndexSession to open any number of block
 * contexts.
 *
 * Close the local index with #udsCloseIndexSession after all contexts for it
 * have been closed.
 *
 * @param [in] name      The name of the index
 * @param [out] session  The name of the new local index session
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsRebuildLocalIndex(const char *name, UdsIndexSession *session);

/**
 * The structure that holds a grid configuration.
 **/
typedef struct udsGridConfig *UdsGridConfig;

/**
 * Creates a new grid configuration.
 *
 * @param [out] gridConfig  The new grid configuration
 *
 * @return                  Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsInitializeGridConfig(UdsGridConfig *gridConfig);

/**
 * Frees a grid configuration.
 *
 * @param [in,out] gridConfig           The grid configuration to free
 **/
void udsFreeGridConfig(UdsGridConfig gridConfig);

/**
 * Adds a server name and port to an existing grid configuration.
 *
 * @param [in,out] gridConfig    The grid configuration being modified
 * @param [in] host              The server running the UDS Grid server (albserver)
 * @param [in] port              The port number to connect to
 *
 * @return                       Either #UDS_SUCCESS or an error
 *                               code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsAddGridServer(UdsGridConfig gridConfig,
                     const char *host,
                     const char *port);


/**
 * Closes an index session.
 *
 * Flushes and saves changes to the index.  Use #udsCloseIndexSession
 * for index sessions created by #udsCreateLocalIndex, #udsLoadLocalIndex,
 * or #udsRebuildLocalIndex.
 *
 * The underlying local index is flushed and properly checkpointed so that
 * #udsLoadLocalIndex can re-open it.
 *
 * Any contexts opened for this session will no longer be valid, and
 * #UDS_DISABLED will be returned if any subsequent operations are
 * attempted on them.
 *
 * @param [in,out] session The session to close
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsCloseIndexSession(UdsIndexSession session);

/**
 * The possible status that an index server can return.
 **/
typedef enum {
  /** The server's status is unknown. */
  UDS_STATUS_UNKNOWN = 0,
  /** The server is starting up and possibly rebuilding the index. */
  UDS_STATUS_STARTING,
  /** The server is up and ready to receive index requests. */
  UDS_STATUS_ONLINE,
  /** The server is saving in-memory state to persistent storage. */
  UDS_STATUS_STOPPING,
  /** The machine is up but there is no server running. */
  UDS_STATUS_OFFLINE
} UdsGridServerStatus;


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
 * Shutdown the UDS library. This is normally called implicitly
 * when a program using the library exits.
 **/
void udsShutdown(void);

/**
 * Suggested buffer size for udsStringError.
 **/
enum {
  UDS_STRING_ERROR_BUFSIZE = 128
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UDS_H */
