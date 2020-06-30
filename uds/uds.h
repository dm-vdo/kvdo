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
 * $Id: //eng/uds-releases/krusty/src/uds/uds.h#11 $
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

#include "compiler.h"
#include "uds-platform.h"

/**
 * Valid request types as described in callbacks.
 **/
enum uds_callback_type {
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
};

/**
 * Valid types for opening an index.
 **/
enum uds_open_index_type {
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
};

/** General UDS constants. */
enum {
	/** The chunk name size in bytes (128 bits = 16 bytes). */
	UDS_CHUNK_NAME_SIZE = 16,
	/** The maximum metadata size in bytes. */
	UDS_METADATA_SIZE = 16,
};

/**
 *  Type representing memory configuration which is either a positive
 *  integer number of gigabytes or one of the three special constants
 *  for configurations which are smaller than 1 gigabyte.
 **/
typedef unsigned int uds_memory_config_size_t;

extern const uds_memory_config_size_t UDS_MEMORY_CONFIG_256MB;
extern const uds_memory_config_size_t UDS_MEMORY_CONFIG_512MB;
extern const uds_memory_config_size_t UDS_MEMORY_CONFIG_768MB;

/**
 *  The maximum configurable amount of memory.
 **/
extern const uds_memory_config_size_t UDS_MEMORY_CONFIG_MAX;

/** The name (hash) of a chunk. */
struct uds_chunk_name {
	/** The name (hash) of a chunk. */
	unsigned char name[UDS_CHUNK_NAME_SIZE];
};

/**
 * Metadata to associate with a chunk name.
 **/
struct uds_chunk_data {
	unsigned char data[UDS_METADATA_SIZE];
};

/**
 * An active index session.
 **/
struct uds_index_session;

/**
 * The data used to configure a new index.
 **/
struct uds_configuration;
typedef uint64_t uds_nonce_t;

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
struct uds_index_stats {
	/** The total number of chunk names stored in the index */
	uint64_t entries_indexed;
	/** An estimate of the index's memory usage */
	uint64_t memory_used;
	/** The number of collisions recorded in the master index */
	uint64_t collisions;
	/** The number of entries discarded from the index since index startup
	 */
	uint64_t entries_discarded;
	/** The number of checkpoints done this session */
	uint64_t checkpoints;
};

/**
 * Context statistics
 *
 * These statistics capture a library context's characteristics either since
 * it was initialized or since its statistics were last reset, whichever
 * is more recent.
 **/
struct uds_context_stats {
	/** The time at which context statistics were last fetched */
	int64_t current_time;
	/**
	 * The number of post calls since context statistics were last reset
	 * that found an existing entry
	 **/
	uint64_t posts_found;
	/**
	 * The number of post calls since context statistics were last reset
	 * that added an entry
	 **/
	uint64_t posts_not_found;
	/**
	 * The number of post calls since context statistics were last reset
	 * that found an existing entry is current enough to only exist in
	 * memory and not have been commited to disk yet.
	 **/
	uint64_t in_memory_posts_found;
	/**
	 * The number of post calls since context statistics were last reset
	 * that found an existing entry in the dense portion of the index.
	 **/
	uint64_t dense_posts_found;
	/**
	 * The number of post calls since context statistics were last reset
	 * that found an existing entry in the sparse portion of the index (if
	 * one exists).
	 **/
	uint64_t sparse_posts_found;
	/**
	 * The number of update calls since context statistics were last reset
	 * that updated an existing entry
	 **/
	uint64_t updates_found;
	/**
	 * The number of update calls since context statistics were last reset
	 * that added a new entry
	 **/
	uint64_t updates_not_found;
	/**
	 * The number of delete requests since context statistics were last
	 * reset that deleted an existing entry
	 **/
	uint64_t deletions_found;
	/**
	 * The number of delete requests since context statistics were last
	 * reset that did nothing.
	 **/
	uint64_t deletions_not_found;
	/**
	 * The number of query calls since context statistics were last reset
	 * that found existing entry
	 **/
	uint64_t queries_found;
	/**
	 * The number of query calls since context statistics were last reset
	 * that did not find an entry
	 **/
	uint64_t queries_not_found;
	/**
	 * The total number of library requests (the sum of posts, updates,
	 * deletions, and queries) since context
	 * statistics were last reset
	 **/
	uint64_t requests;
};

struct uds_request;

/**
 * Callback function invoked to inform the Application Software that an
 * operation started by #uds_start_chunk_operation has completed.
 *
 * @param [in] request  The operation that finished.  When the callback
 *                      function is called, this uds_request structure can be
 *                      reused or freed.
 **/
typedef void uds_chunk_callback_t(struct uds_request *request);

/**
 * Request structure passed to #uds_start_chunk_operation to begin an
 * operation, and returned to the Application Software when the callback
 * function is invoked.
 **/
struct uds_request {
	/*
	 * The name of the block.
	 * Set before starting an operation.
	 * Unchanged at time of callback.
	 */
	struct uds_chunk_name chunk_name;
	/*
	 * The metadata found in the index that was associated with the block
	 * (sometimes called the canonical address).
	 * Set before the callback.
	 */
	struct uds_chunk_data old_metadata;
	/*
	 * The new metadata to associate with the name of the block (sometimes
	 * called the duplicate address). Set before starting a #UDS_POST or
	 * #UDS_QUERY operation. Unchanged at time of callback.
	 */
	struct uds_chunk_data new_metadata;
	/*
	 * The callback method to be invoked when the operation finishes.
	 * Set before starting an operation.
	 * Unchanged at time of callback.
	 */
	uds_chunk_callback_t *callback;
	/*
	 * The index session.
	 * Set before starting an operation.
	 * Unchanged at time of callback.
	 */
	struct uds_index_session *session;
	/*
	 * The operation type, which is one of #UDS_DELETE, #UDS_POST,
	 * #UDS_QUERY or #UDS_UPDATE. Set before starting an operation.
	 * Unchanged at time of callback.
	 */
	enum uds_callback_type type;
	/*
	 * The operation status, which is either #UDS_SUCCESS or an error code.
	 * Set before the callback.
	 */
	int status;
	/*
	 * If true, the name of the block was found in the index.
	 * Set before the callback.
	 */
	bool found;
	/*
	 * If true, move the entry to the end of the deduplication window.
	 * Set before starting a #UDS_QUERY operation.
	 * Unchanged at time of callback.
	 */
	bool update;
	long private[25];
};

/**
 * Initializes an index configuration.
 *
 * @param [out] conf          The new configuration
 * @param [in] mem_gb          The maximum memory allocation, in GB
 *
 * @return                    Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_initialize_configuration(struct uds_configuration **conf,
					      uds_memory_config_size_t mem_gb);

/**
 * Sets or clears an index configuration's sparse indexing settings.
 *
 * @param [in,out] conf       The configuration to change
 * @param [in] sparse         If <code>true</code>, request a sparse
 *                            index; if <code>false</code>, request
 *                            a default index.
 *
 **/
void uds_configuration_set_sparse(struct uds_configuration *conf, bool sparse);

/**
 * Tests whether an index configuration specifies sparse indexing.
 *
 * @param [in] conf           The configuration to check
 *
 * @return                    Returns <code>true</code> if the configuration
 *                            is sparse, or <code>false</code> if not
 **/
bool __must_check uds_configuration_get_sparse(struct uds_configuration *conf);

/**
 * Sets an index configuration's nonce.
 *
 * @param [in,out] conf  The configuration to change
 * @param [in] nonce    The 64 bit nonce.
 *
 **/
void uds_configuration_set_nonce(struct uds_configuration *conf,
				 uds_nonce_t nonce);

/**
 * Gets an index configuration's nonce.
 *
 * @param [in] conf  The configuration to check
 *
 * @return  The 64 bit nonce.
 **/
uds_nonce_t __must_check
uds_configuration_get_nonce(struct uds_configuration *conf);

/**
 * Fetches a configuration's maximum memory allocation.
 *
 * @param [in] conf  The configuration to check
 *
 * @return      The amount of memory allocated, in GB
 **/
uds_memory_config_size_t __must_check
uds_configuration_get_memory(struct uds_configuration *conf);

/**
 * Fetches a configuration's chapters per volume value.
 *
 * @param [in] conf  The configuration to check
 *
 * @return      The number of chapters per volume
 **/
unsigned int __must_check
uds_configuration_get_chapters_per_volume(struct uds_configuration *conf);

/**
 * Frees memory used by a configuration.
 *
 * @param [in,out] conf The configuration for which memory is being freed
 **/
void uds_free_configuration(struct uds_configuration *conf);

/**
 * Compute the size required to store the index on persistent storage.  This
 * size is valid for any index stored in a single file or on a single block
 * device.  This size should be used when configuring a block device on which
 * to store an index.
 *
 * @param [in]  config          A uds_configuration for an index.
 * @param [in]  num_checkpoints  The maximum number of checkpoints.
 * @param [out] index_size       The number of bytes required to store
 *                              the index.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check uds_compute_index_size(const struct uds_configuration *config,
					unsigned int num_checkpoints,
					uint64_t *index_size);

/**
 * Opens an index session.
 *
 * Creates a session for an index. #uds_open_index must be called before
 * the index can be used.
 *
 * Destroy the session with #uds_destroy_index_session.
 *
 * @param [out] session  A pointer to the new session
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_create_index_session(struct uds_index_session **session);

/**
 * Fetches the UDS library version.
 *
 * @return       The library version
 **/
const char * __must_check uds_get_version(void);

/**
 * The name argument to #uds_open_index is a text string that names the index.
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
 * The index should be closed with #uds_close_index.
 *
 * @param open_type  The type of open, which is one of #UDS_LOAD, #UDS_CREATE,
 *                  or #UDS_NO_REBUILD.
 * @param name      The name of the index
 * @param params    The index session parameters.  If NULL, the default
 *                       session parameters will be used.
 * @param conf      The index configuration
 * @param session   The index session
 *
 * @return          Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_open_index(enum uds_open_index_type open_type,
				const char *name,
				const struct uds_parameters *params,
				struct uds_configuration *conf,
				struct uds_index_session *session);

/**
 * Waits until all callbacks for index operations are complete, and prevents
 * new index operations from starting. Index operations will return
 * UDS_SUSPENDED until #uds_resume_index_session is called. Optionally saves
 * all index data before returning.
 *
 * @param session  The session to suspend
 * @param save     Whether to save index data
 *
 * @return  Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_suspend_index_session(struct uds_index_session *session,
					   bool save);

/**
 * Allows new index operations for an index, whether it was suspended or not.
 *
 * @param session  The session to resume
 *
 * @return  Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_resume_index_session(struct uds_index_session *session);

/**
 * Waits until all callbacks for index operations are complete.
 *
 * @param [in] session  The session to flush
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_flush_index_session(struct uds_index_session *session);

/**
 * Closes an index.  This operation will fail if the index session is
 * suspended.
 *
 * Saves changes to the index so that #uds_open_index can re-open it.
 *
 * @param [in] session  The session containing the index to close
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_close_index(struct uds_index_session *session);

/**
 * Destroys an index session.
 *
 * Saves changes to the index and closes the index if one is open.
 * Use #uds_destroy_index_session for index sessions created by
 * #uds_create_index_session.
 *
 * @param [in] session  The session to destroy
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
int uds_destroy_index_session(struct uds_index_session *session);

/**
 * Returns the configuration for the given index session.
 *
 * @param [in]  session The session
 * @param [out] conf    The index configuration
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_get_index_configuration(struct uds_index_session *session,
					     struct uds_configuration **conf);

/**
 * Fetches index statistics for the given index session.
 *
 * @param [in]  session The session
 * @param [out] stats   The index statistics structure to fill
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_get_index_stats(struct uds_index_session *session,
				     struct uds_index_stats *stats);

/**
 * Fetches index session statistics for the given index session.
 *
 * @param [in]  session  The session
 * @param [out] stats    The context statistics structure to fill
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_get_index_session_stats(struct uds_index_session *session,
					     struct uds_context_stats *stats);

/**
 * Convert an error code to a string.
 *
 * @param errnum       The error code
 * @param buf          The buffer to hold the error string
 * @param buflen       The length of the buffer
 *
 * @return A pointer to buf
 **/
const char * __must_check uds_string_error(int errnum,
					   char *buf,
					   size_t buflen);

/**
 * Suggested buffer size for uds_string_error.
 **/
enum { UDS_STRING_ERROR_BUFSIZE = 128 };

/** @{ */
/** @name Deduplication */

/**
 * Start a UDS index chunk operation.  The request <code>type</code> field must
 * be set to the type of operation.  This is an asynchronous interface to the
 * block-oriented UDS API.  The callback is invoked upon completion.
 *
 * The #UDS_DELETE operation type deletes the mapping for a particular block.
 * #UDS_DELETE is typically used when UDS provides invalid advice.
 *
 * The #UDS_POST operation type indexes a block name and associates it with a
 * particular address.  The caller provides the block's name. UDS then checks
 * this name against its index.
 * <ul>
 *   <li>If the block is new, it is stored in the index.</li>
 *   <li>If the block is a duplicate of an indexed block, UDS returns the
 *       canonical block address via the callback.</li>
 * </ul>
 *
 * The #UDS_QUERY operation type checks to see if a block name exists in the
 * index.  The caller provides the block's name.  UDS then checks
 * this name against its index.
 * <ul>
 *   <li>If the block is new, no action is taken.</li>

 *   <li>If the block is a duplicate of an indexed block, UDS returns the
 *       canonical block address via the callback.  If the <code>update</code>
 *       field is set, the entry is moved to the end of the deduplication
 *       window.</li> </ul>
 *
 * The #UDS_UPDATE operation type updates the mapping for a particular block.
 * #UDS_UPDATE is typically used if the callback function provides invalid
 * advice.
 *
 * @param [in] request  The operation.  The <code>type</code>,
 *                      <code>chunk_name</code>, <code>new_metadata</code>,
 *                      <code>context</code>, <code>callback</code>, and
 *                      <code>update</code> fields must be set.  At callback
 *                      time, the <code>old_metadata</code>,
 *                      <code>status</code>, and <code>found</code> fields will
 *                      be set.
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_start_chunk_operation(struct uds_request *request);
/** @} */

#endif /* UDS_H */
