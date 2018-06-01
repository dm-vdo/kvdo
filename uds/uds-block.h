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
 * $Id: //eng/uds-releases/gloria/src/public/uds-block.h#2 $
 */

/**
 * @file
 * @brief Definitions for the UDS block interface
 **/
#ifndef UDS_BLOCK_H
#define UDS_BLOCK_H

#include "uds.h"

/** General UDS block constants. */
enum {
  /** The maximum metadata size for a block. */
  UDS_MAX_BLOCK_DATA_SIZE = UDS_MAX_METADATA_SIZE
};

/**
 * A UDS block context.
 **/
typedef struct udsBlockContext {
  /** The context ID. */
  unsigned int id;
} UdsBlockContext;

/**
 * Metadata to associate with a blockName.
 **/
struct udsChunkData {
  unsigned char data[UDS_MAX_BLOCK_DATA_SIZE];
};

/**
 * Represents a block address on disk.
 *
 * #UdsBlockAddress objects allow the Application Software and UDS
 * to refer to specific disk blocks.  It might be, for instance, the
 * logical block address divided by the block size.
 *
 * These objects are stored persistently in the index and are also cached.
 * Therefore, make every effort to ensure that these objects are as small as
 * possible.
 **/
typedef void *UdsBlockAddress;
/** @{ */
/** @name Context Management */

/**
 * Opens a new block context for an index session.
 *
 * You must first create the index session with #udsCreateLocalIndex,
 * #udsLoadLocalIndex, or #udsRebuildLocalIndex.
 *
 * The Application Software can use this context for any number of operations
 * while the underlying index session is active. Call #udsCloseBlockContext to
 * close the context.
 *
 * If a fatal error occurs on either the context or the underlying index
 * session, or if the index session has been closed, all subsequent operations
 * will return #UDS_DISABLED.  In that case, close the block context, close the
 * index session, and then reload or rebuild the index. Then call
 * #udsOpenBlockContext to open a fresh block context.
 *
 * @param [in] session       The index session
 * @param [in] metadataSize  This value is unused
 * @param [out] context      The new block context
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsOpenBlockContext(UdsIndexSession  session,
                        unsigned int     metadataSize,
                        UdsBlockContext *context);

/**
 * Closes a block context.
 *
 * #udsCloseBlockContext flushes and saves work in that context
 * before closing it.
 *
 * @param [in] context  The block context to close
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsCloseBlockContext(UdsBlockContext context);

/**
 * Waits until all callbacks for index operations are complete.
 *
 * @param [in] context  The block context to flush
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsFlushBlockContext(UdsBlockContext context);
/** @} */

/** @{ */
/** @name Deduplication */

typedef struct udsRequest UdsRequest;

/**
 * Callback function invoked to inform the Application Software that an
 * operation started by #udsStartChunkOperation has completed.
 *
 * @param [in] request  The operation that finished.  When the callback
 *                      function is called, this UdsRequest structure can be
 *                      reused or freed.
 **/
typedef void UdsChunkCallback(UdsRequest *request);

/**
 * Request structure passed to #udsStartChunkOperation to begin an operation,
 * and returned to the Application Software when the callback function is
 * invoked.
 **/
struct udsRequest {
  /*
   * The name of the block.
   * Set before starting an operation.
   * Unchanged at time of callback.
   */
  UdsChunkName chunkName;
  /*
   * The metadata found in the index that was associated with the block
   * (sometimes called the canonical address).
   * Set before the callback.
   */
  struct udsChunkData oldMetadata;
  /*
   * The new metadata to associate with the name of the block (sometimes called
   * the duplicate address).
   * Set before starting a #UDS_POST or #UDS_QUERY operation.
   * Unchanged at time of callback.
   */
  struct udsChunkData newMetadata;
  /*
   * The callback method to be invoked when the operation finishes.
   * Set before starting an operation.
   * Unchanged at time of callback.
   */
  UdsChunkCallback *callback;
  /*
   * The block context.
   * Set before starting an operation.
   * Unchanged at time of callback.
   */
  UdsBlockContext context;
  /*
   * The operation type, which is one of #UDS_DELETE, #UDS_POST, #UDS_QUERY or
   * #UDS_UPDATE.
   * Set before starting an operation.
   * Unchanged at time of callback.
   */
  UdsCallbackType type;
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
 *                      <code>chunkName</code>, <code>newMetadata</code>,
 *                      <code>context</code>, <code>callback</code>, and
 *                      <code>update</code> fields must be set.  At callback
 *                      time, the <code>oldMetadata</code>,
 *                      <code>status</code>, and <code>found</code> fields will
 *                      be set.
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsStartChunkOperation(UdsRequest *request);
/** @} */

/** @{ */
/** @name Monitoring */

/**
 * Fetches index statistics for the given context.
 *
 * @param [in]  context The block context
 * @param [out] stats   The index statistics structure to fill
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsGetBlockContextIndexStats(UdsBlockContext  context,
                                 UdsIndexStats   *stats);

/**
 * Fetches context statistics for the given context.
 *
 * @param [in]  context The block context
 * @param [out] stats   The context statistics structure to fill
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsGetBlockContextStats(UdsBlockContext  context,
                            UdsContextStats *stats);

/** @} */

#endif /* UDS_BLOCK_H */
