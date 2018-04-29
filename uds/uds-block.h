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
 * $Id: //eng/uds-releases/flanders-rhel7.5/src/public/uds-block.h#1 $
 */

/**
 * @file
 * @brief Definitions for the UDS block interface
 **/
#ifndef UDS_BLOCK_H
#define UDS_BLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

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

/**
 * Callback function invoked to inform the Application Software that an
 * operation completed.
 *
 * The <code>context</code>, <code>type</code>, <code>status</code>,
 * <code>cookie</code>, and <code>callbackArgument</code> parameters
 * are always set.  The rest of them depend on the callback type.
 *
 * <ul>
 * <li> <code>duplicateAddress</code>: is set except for #UDS_DELETE.</li>
 * <li> <code>canonicalAddress</code>: is set if the chunk existed.</li>
 * <li> <code>blockName</code>: is always set.</li>
 * <li> <code>blockLength</code>: is zero unless this is a result
 * of a call to #udsPostBlock.
 * </ul>
 *
 * On a duplicate block callback, retain the canonical address and adjust the
 * duplicate address to share data with the canonical address.
 *
 * All callbacks are invoked in one thread, so the callback function should
 * not block.  In addition, this function should not call exit() or make
 * additional calls into the index (e.g. #udsUpdateBlockMapping).
 *
 * @param [in,out] context      The context
 * @param [in] type             The callback type
 * @param [in] status           The request status (either
 *                              #UDS_SUCCESS or an error code)
 * @param [in] cookie           The opaque data from the call that invoked
 *                              this callback
 * @param [in] duplicateAddress The duplicate address, which can possibly be
 *                              freed
 * @param [in] canonicalAddress The canonical address of the chunk
 * @param [in] blockName        The name (hash) of the chunk being referenced,
 *                              used for unmapping or remapping this block
 * @param [in] blockLength      The block length
 * @param [in] callbackArgument The <code>callbackArgument</code> given when
 *                              registering the callback
 **/
typedef void (*UdsDedupeBlockCallback)
(UdsBlockContext    context,
 UdsCallbackType    type,
 int                status,
 UdsCookie          cookie,
 UdsBlockAddress    duplicateAddress,
 UdsBlockAddress    canonicalAddress,
 UdsChunkName      *blockName,
 size_t             blockLength,
 void              *callbackArgument);

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
 * @param [in] metadataSize  The size of metadata to be passed using the
 *                           context
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

/**
 * Registers a callback to be invoked when asynchronous index operations are
 * complete.
 *
 * The #udsPostBlock, #udsPostBlockName, #udsUpdateBlockMapping, and
 * #udsDeleteBlockMapping functions invoke the registered callback to inform
 * the Application Software that an operation is complete.
 *
 * @param [in] context          The library context
 * @param [in] cb               The callback function, or <code>NULL</code>
 *                              to unregister an existing callback
 * @param [in] callbackArgument Opaque argument for the callback
 *
 * @return                      Either #UDS_SUCCESS or an error code
 *
 * @see #UdsDedupeBlockCallback
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsRegisterDedupeBlockCallback(UdsBlockContext         context,
                                   UdsDedupeBlockCallback  cb,
                                   void                   *callbackArgument);

/**
 * Controls the maximum size of the backlog of pending requests
 * managed within the UDS library.
 *
 * Higher values allow for more parallelism and better amortize
 * processing time (especially when the fingerprint API is not used),
 * but also tends to increase turnaround time of individual requests.
 *
 * @param [in] context          The library context
 * @param [in] maxRequests      The maximum backlog size
 *
 * @return                      Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsSetBlockContextRequestQueueLimit(UdsBlockContext context,
                                        unsigned int    maxRequests);

/**
 * Controls the function used for hashing incoming blocks of data.
 * The list of functions available are confined to the #UdsHashAlgorithm
 * enum. This function performs no locking of the context, so changing
 * the hash algorithm should only be done at open.
 *
 * @param [in] context          The library context
 * @param [in] algorithm        The algorithm to use for hashing blocks
 *                              submitted through #udsPostBlock.
 *
 * @return                      Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsSetBlockContextHashAlgorithm(UdsBlockContext  context,
                                    UdsHashAlgorithm algorithm);

/** @} */

/** @{ */
/** @name Deduplication */

/**
 * Indexes a block and asynchronously associates it with a particular address;
 * if a callback function has been registered, it is invoked upon completion.
 *
 * This is an asynchronous interface to the block-oriented UDS
 * API. The Application Software provides the block's name. UDS then
 * checks this name against its index.
 * <ul>
 *   <li>If the block is new, it is stored in the index.</li>
 *   <li>If the block is a duplicate of an indexed block, UDS returns the
 *       canonical block address via the #UdsDedupeBlockCallback callback.</li>
 * </ul>
 *
 * @param [in] context      The library context
 * @param [in] cookie       Opaque data for the callback
 * @param [in] blockAddress The address of the block being referenced
 * @param [in] dataLength   The length of the block data
 * @param [in] data         The block data (the UDS library will not access
 *                          it once this function returns)
 *
 * @return                  Either #UDS_SUCCESS or an error code
 *
 * @see #udsRegisterDedupeBlockCallback
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsPostBlock(UdsBlockContext  context,
                 UdsCookie        cookie,
                 UdsBlockAddress  blockAddress,
                 size_t           dataLength,
                 const void      *data);

/**
 * Indexes a block name and asynchronously associates it with a particular
 * address;  if a callback function has been registered, it is invoked upon
 * completion.
 *
 * This is an asynchronous interface to the block-oriented UDS
 * API.  The Application Software provides the block's name. UDS then
 * checks this name against its index.
 * <ul>
 *   <li>If the block is new, it is stored in the index.</li>
 *   <li>If the block is a duplicate of an indexed block, UDS returns the
 *       canonical block address via the #UdsDedupeBlockCallback callback.</li>
 * </ul>
 *
 * @param [in] context      The context for the block
 * @param [in] cookie       Opaque data for the callback
 * @param [in] blockAddress The address of the block being referenced
 * @param [in] chunkName    The name of the block
 *
 * @return                  Either #UDS_SUCCESS or an error code
 *
 * @see #udsRegisterDedupeBlockCallback
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsPostBlockName(UdsBlockContext     context,
                     UdsCookie           cookie,
                     UdsBlockAddress     blockAddress,
                     const UdsChunkName *chunkName);

/**
 * Checks to see if a block name exists in the index.
 *
 * The Application Software provides the block's name. UDS then checks
 * this name against its index. The Application Software may optionally
 * supply a source block address; this is returned in the callback but
 * not otherwise used.
 * <ul>
 *   <li>If the block is new, no action is taken.</li>
 *   <li>If the block is a duplicate of an indexed block, UDS returns the
 *       canonical block address via the #UdsDedupeBlockCallback callback.</li>
 * </ul>
 *
 * @param [in] context      The library context
 * @param [in] cookie       Opaque data for the callback
 * @param [in] blockAddress The address of the block being referenced;
 *                          may be <code>NULL</code>
 * @param [in] blockName    The block mapping to check
 * @param [in] update       If true, move the entry to the
 *                          end of the deduplication window
 *                          if found.
 *
 * @return                  Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsQueryBlockName(UdsBlockContext     context,
                      UdsCookie           cookie,
                      UdsBlockAddress     blockAddress,
                      const UdsChunkName *blockName,
                      bool                update);

/**
 * Checks to see if a block exists in the index.
 *
 * The Application Software provides the block's data. UDS then checks
 * this block against its index. The Application Software may optionally
 * supply a source block address; this is returned in the callback but
 * not otherwise used.
 * <ul>
 *   <li>If the block is new, no action is taken.</li>
 *   <li>If the block is a duplicate of an indexed block, UDS returns the
 *       canonical block address via the #UdsDedupeBlockCallback callback.</li>
 * </ul>
 *
 * @param [in] context      The library context
 * @param [in] cookie       Opaque data for the callback
 * @param [in] blockAddress The address of the block being referenced;
 *                          may be <code>NULL</code>
 * @param [in] dataLength   The length of the block data
 * @param [in] data         The block data (the UDS library will not access
 *                          it once this function returns)
 * @param [in] update       If true, move the entry to the
 *                          end of the deduplication window
 *                          if found.
 *
 * @return                  Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsCheckBlock(UdsBlockContext  context,
                  UdsCookie        cookie,
                  UdsBlockAddress  blockAddress,
                  size_t           dataLength,
                  const void      *data,
                  bool             update);

/**
 * Updates the mapping for a particular block.  This operation occurs
 * asynchronously.
 *
 * #udsUpdateBlockMapping is typically called if #UdsDedupeBlockCallback
 * provides invalid advice, but the <code>duplicateAddress</code> remains
 * valid on disk. This call updates the <code>canonicalAddress</code> for this
 * <code>blockName</code> and then invokes the #UdsDedupeBlockCallback to
 * inform the Application Software that the operation is complete.
 *
 * @param [in] context      The library context
 * @param [in] cookie       Opaque data for the callback
 * @param [in] blockName    The block mapping to update
 * @param [in] blockAddress The new canonical mapping for this
 *                          <code>blockName</code>
 *
 * @return                  Either #UDS_SUCCESS or an error code
 *
 * @see #udsRegisterDedupeBlockCallback
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsUpdateBlockMapping(UdsBlockContext     context,
                          UdsCookie           cookie,
                          const UdsChunkName *blockName,
                          UdsBlockAddress     blockAddress);

/**
 * Deletes the mapping for a particular block.  This operation occurs
 * asynchronously.
 *
 * #udsDeleteBlockMapping is typically called if #UdsDedupeBlockCallback
 * provides invalid advice, and neither the <code>duplicateAddress</code> nor
 * the <code>canonicalAddress</code> still exist. This function invokes
 * the #UdsDedupeBlockCallback callback to inform the Application Software
 * when the operation is complete.
 *
 * @param [in] context    The library context
 * @param [in] cookie     Opaque data for the callback
 * @param [in] blockName  The block mapping to delete
 *
 * @return                Either #UDS_SUCCESS or an error code
 *
 * @see #udsRegisterDedupeBlockCallback
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsDeleteBlockMapping(UdsBlockContext     context,
                          UdsCookie           cookie,
                          const UdsChunkName *blockName);

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
 * Returns the configuration for the given context.
 *
 * @param [in]  context The block context
 * @param [out] conf    The index configuration
 *
 * @return              Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsGetBlockContextConfiguration(UdsBlockContext   context,
                                    UdsConfiguration *conf);

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

/**
 * Resets context statistics for the given context.
 *
 * @param [in] context The block context to reset
 *
 * @return             Either #UDS_SUCCESS or an error code
 **/
UDS_ATTR_WARN_UNUSED_RESULT
int udsResetBlockContextStats(UdsBlockContext context);

/** @} */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UDS_BLOCK_H */
