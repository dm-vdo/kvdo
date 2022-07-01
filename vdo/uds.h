/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

/**
 * @mainpage UDS API Reference
 * <center>Copyright Red Hat</center>
 **/

/**
 * @file
 * @brief General UDS definitions
 **/
#ifndef UDS_H
#define UDS_H

#include <linux/types.h>

#include "compiler.h"
#include "funnel-queue.h"

/**
 * Valid request types.
 **/
enum uds_request_type {
	/**
	 * Request type for operations that post mappings to the UDS
	 * index.  When the chunk-hash being added already exists, the
	 * existing metadata is not overwritten. Regardless, the
	 * recency of the chunk is updated.
	 **/
	UDS_POST,

	/**
	 * Request type for operations that update mappings in the UDS
	 * index. If the indicated entry does not have any mapping in the
	 * index, one is created. In either case, the recency of
	 * the chunk is updated.
	 **/
	UDS_UPDATE,

	/**
	 * Request type for operations that delete mappings from the
	 * UDS index. */
	UDS_DELETE,

	/**
	 * Request type for operations that query mappings in the UDS
	 * index. The recency of the mapping is updated.
	 **/
	UDS_QUERY,

	/**
	 * Request type for operations that query mappings in the UDS
	 * index without updating the recency of the mapping.
	 **/
	UDS_QUERY_NO_UPDATE,
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
 *  integer number of gigabytes or one of the six special constants
 *  for configurations which are smaller than 1 gigabyte.
 **/
typedef int uds_memory_config_size_t;

enum {
	/*  The maximum configurable amount of memory. */
	UDS_MEMORY_CONFIG_MAX = 1024,
	/*  Flag indicating volume has one less chapter than usual */
	UDS_MEMORY_CONFIG_REDUCED = 0x1000,
	/*  Flag indicating volume has one less chapter than usual */
	UDS_MEMORY_CONFIG_REDUCED_MAX = 1024 + UDS_MEMORY_CONFIG_REDUCED,
	/*  Special values indicating sizes less than 1 GB */
	UDS_MEMORY_CONFIG_256MB = -256,
	UDS_MEMORY_CONFIG_512MB = -512,
	UDS_MEMORY_CONFIG_768MB = -768,
	UDS_MEMORY_CONFIG_REDUCED_256MB = -1280,
	UDS_MEMORY_CONFIG_REDUCED_512MB = -1536,
	UDS_MEMORY_CONFIG_REDUCED_768MB = -1792,
};

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
struct uds_parameters {
	/** String describing the storage device */
	const char *name;
	/** The maximum allowable size of the index on storage */
	size_t size;
	/** The offset where the index should start */
	off_t offset;
	/** The maximum memory allocation, in GB */
	uds_memory_config_size_t memory_size;
	/** Whether the index should include sparse chapters */
	bool sparse;
	/** A 64-bit nonce to validate the index */
	uint64_t nonce;
	/** The number of threads used to process index requests */
	unsigned int zone_count;
	/** The number of threads used to read volume pages */
	unsigned int read_threads;
};

/**
 * Index statistics
 *
 * These statistics capture the current index characteristics, including
 * resource usage and requests processed since initialization.
 **/
struct uds_index_stats {
	/** The total number of chunk names stored in the index. */
	uint64_t entries_indexed;
	/** An estimate of the index's memory usage. */
	uint64_t memory_used;
	/** The number of collisions recorded in the volume index. */
	uint64_t collisions;
	/** The number of entries discarded from the index since startup. */
	uint64_t entries_discarded;
	/** The time at which these statistics were fetched. */
	int64_t current_time;
	/** The number of post calls that found an existing entry. */
	uint64_t posts_found;
	/** The number of post calls that added an entry. */
	uint64_t posts_not_found;
	/**
	 * The number of post calls that found an existing entry that is
	 * current enough to only exist in memory and not have been committed
	 * to disk yet.
	 **/
	uint64_t in_memory_posts_found;
	/**
	 * The number of post calls that found an existing entry in the dense
	 * portion of the index.
	 **/
	uint64_t dense_posts_found;
	/**
	 * The number of post calls that found an existing entry in the sparse
	 * portion of the index (if one exists).
	 **/
	uint64_t sparse_posts_found;
	/** The number of update calls that updated an existing entry. */
	uint64_t updates_found;
	/** The number of update calls that added a new entry. */
	uint64_t updates_not_found;
	/** The number of delete requests that deleted an existing entry. */
	uint64_t deletions_found;
	/** The number of delete requests that did nothing. */
	uint64_t deletions_not_found;
	/** The number of query calls that found existing entry. */
	uint64_t queries_found;
	/** The number of query calls that did not find an entry. */
	uint64_t queries_not_found;
	/**
	 * The total number of library requests (the sum of posts, updates,
	 * deletions, and queries).
	 **/
	uint64_t requests;
};

/**
 * Internal index structure.
 **/
struct uds_index;

/**
 * The block's general location in the index.
 **/
enum uds_index_region {
	/* no location information has been determined */
	UDS_LOCATION_UNKNOWN = 0,
	/* the index page entry has been found */
	UDS_LOCATION_INDEX_PAGE_LOOKUP,
	/* the record page entry has been found */
	UDS_LOCATION_RECORD_PAGE_LOOKUP,
	/* the block is not in the index */
	UDS_LOCATION_UNAVAILABLE,
	/* the block was found in the open chapter */
	UDS_LOCATION_IN_OPEN_CHAPTER,
	/* the block was found in the dense part of the index */
	UDS_LOCATION_IN_DENSE,
	/* the block was found in the sparse part of the index */
	UDS_LOCATION_IN_SPARSE
} __packed;

/**
 * enum uds_zone_message_type indicates what kind of zone message (if any)
 * is contained in this request.
 **/
enum uds_zone_message_type {
	/** A standard request with no message */
	UDS_MESSAGE_NONE = 0,
	/** Add a chapter to the sparse chapter index cache */
	UDS_MESSAGE_SPARSE_CACHE_BARRIER,
	/** Close a chapter to keep the zone from falling behind */
	UDS_MESSAGE_ANNOUNCE_CHAPTER_CLOSED,
} __packed;

struct uds_zone_message {
	/** The type of message, determining how it will be processed */
	enum uds_zone_message_type type;
	/** The virtual chapter number to which the message applies */
	uint64_t virtual_chapter;
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
	 * #UDS_UPDATE operation. Unchanged at time of callback.
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
	 * The operation type, which is one of #UDS_POST, #UDS_UPDATE,
	 * #UDS_DELETE, #UDS_QUERY or #UDS_QUERY_NO_UPDATE.
	 * Set before starting an operation.
	 * Unchanged at time of callback.
	 */
	enum uds_request_type type;
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
	 * The remainder of this structure consists of fields used within the
	 * index to manage its operations. Clients should not set or alter
	 * the values of these fields. We rely on zone_number being the first
	 * field in this section.
	 */

	/** The number of the zone which will handle this */
	unsigned int zone_number;
	/** A link for adding a request to a lock-free queue */
	struct funnel_queue_entry request_queue_link;
	/** A link for adding a request to a standard linked list */
	struct uds_request *next_request;
	/** A pointer to the index handling this request */
	struct uds_index *index;
	/** Zone control message for coordinating between zones */
	struct uds_zone_message zone_message;
	/** If true, handle request immediately by waking the worker thread */
	bool unbatched;
	/** If true, attempt to handle this request before newer requests */
	bool requeued;
	/** The virtual chapter containing the record */
	uint64_t virtual_chapter;
	/** The location of this chunk name in the index */
	enum uds_index_region location;
};

/**
 * Compute the size required to store the index on persistent storage.  This
 * size is valid for any index stored in a single file or on a single block
 * device.  This size should be used when configuring a block device on which
 * to store an index.
 *
 * @param [in]  parameters  Parameters for an index.
 * @param [out] index_size  The number of bytes required to store the index.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check uds_compute_index_size(const struct uds_parameters *parameters,
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
 * Opens an index with an existing session.  This operation will fail if the
 * index session is suspended, or if there is already an open index.
 *
 * The index should be closed with #uds_close_index.
 *
 * @param open_type   The type of open, which is one of #UDS_LOAD, #UDS_CREATE,
 *                    or #UDS_NO_REBUILD.
 * @param parameters  The index parameters
 * @param session     The index session
 *
 * @return          Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_open_index(enum uds_open_index_type open_type,
				const struct uds_parameters *parameters,
				struct uds_index_session *session);

/**
 * Waits until all callbacks for index operations are complete, and prevents
 * new index operations from starting. Index operations will return -EBUSY
 * until #uds_resume_index_session is called. Optionally saves all index data
 * before returning.
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
 * If the index is suspended and the supplied path is different from the
 * current backing store, the index will start using the new backing store.
 *
 * @param session  The session to resume
 * @param name     A name describing the new backing store to use
 *
 * @return  Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_resume_index_session(struct uds_index_session *session,
					  const char *name);

/**
 * Waits until all callbacks for index operations are complete.
 *
 * @param [in] session  The session to flush
 *
 * @return Either #UDS_SUCCESS or an error code
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
 * Returns the parameters for the given index session. The caller is
 * responsible for freeing the returned structure.
 *
 * @param [in]  session     The session
 * @param [out] parameters  A copy of the index parameters
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_get_index_parameters(struct uds_index_session *session,
					  struct uds_parameters **parameters);

/**
 * Fetches index statistics for the given index session.
 *
 * @param [in]  session  The session
 * @param [out] stats    The index statistics structure to fill
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_get_index_stats(struct uds_index_session *session,
				     struct uds_index_stats *stats);

/** @{ */
/** @name Deduplication */

/**
 * Start a UDS index chunk operation. The request <code>type</code> field must
 * be set to the type of operation. This is an asynchronous interface to the
 * block-oriented UDS API. The callback is invoked upon completion.
 *
 * The #UDS_POST operation type indexes a block name and associates it with a
 * particular address. The caller provides the block's name. UDS then checks
 * this name against its index.
 * <ul>
 *   <li>If the block is new, it is stored in the index.</li>
 *   <li>If the block is a duplicate of an indexed block, UDS returns the
 *       canonical block address via the callback.</li>
 * </ul>
 *
 * The #UDS_UPDATE operation type updates the mapping for a particular block.
 * #UDS_UPDATE is typically used if the callback function provides invalid
 * advice.
 *
 * The #UDS_DELETE operation type deletes the mapping for a particular block.
 * #UDS_DELETE is typically used when UDS provides invalid advice.
 *
 * The #UDS_QUERY operation type checks to see if a block name exists in the
 * index. The caller provides the block's name. UDS then checks this name
 * against its index.
 * <ul>
 *   <li>If the block is new, no action is taken.</li>
 *   <li>If the block is a duplicate of an indexed block, UDS returns the
 *       canonical block address via the callback and the entry is moved to
 *       the end of the deduplication window.</li> </ul>
 *
 * The #UDS_QUERY_NO_UPDATE operation type checks to see if a block name exists
 * in the index. The caller provides the block's name. UDS then checks this
 * name against its index.
 * <ul>
 *   <li>If the block is new, no action is taken.</li>
 *   <li>If the block is a duplicate of an indexed block, UDS returns the
 *       canonical block address via the callback.
 *
 * @param [in] request  The operation.  The <code>type</code>,
 *                      <code>chunk_name</code>, <code>new_metadata</code>,
 *                      <code>context</code>, <code>callback</code>, and
 *                      <code>update</code> fields must be set.  At callback
 *                      time, the <code>old_metadata</code>,
 *                      <code>status</code>, and <code>found</code> fields will
 *                      be set.
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
int __must_check uds_start_chunk_operation(struct uds_request *request);
/** @} */

#endif /* UDS_H */
