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
 * $Id: //eng/uds-releases/flanders/src/uds/block.c#2 $
 */

#include "uds-block.h"

#include "context.h"
#include "featureDefs.h"
#include "notificationDefs.h"
#include "request.h"
#include "uds-error.h"

static void handleBlockCallback(Request *request);

/**
 * Encode block metadata.
 *
 * @param metadata     The metadata to encode
 * @param request      The request in which to encode the metadata
 **/
static void encodeBlockMetadata(const void *metadata, Request *request)
{
  memcpy(request->newMetadata.data, metadata, request->context->metadataSize);
}

#if NAMESPACES
/**********************************************************************/
int udsOpenBlockContext(UdsIndexSession     session,
                        const UdsNamespace *namespace,
                        unsigned int        metadataSize,
                        UdsBlockContext    *context)
{
  if (context == NULL) {
    return UDS_CONTEXT_PTR_REQUIRED;
  }
  int result = openContext(session, namespace, metadataSize,
                           encodeBlockMetadata, 0, BLOCK_CONTEXT,
                           handleBlockCallback, &context->id);
  if (result == UDS_SUCCESS) {
    notifyBlockContextOpened(session, *context);
  }
  return result;
}
#else
/**********************************************************************/
int udsOpenBlockContext(UdsIndexSession     session,
                        unsigned int        metadataSize,
                        UdsBlockContext    *context)
{
  if (context == NULL) {
    return UDS_CONTEXT_PTR_REQUIRED;
  }
  int result = openContext(session, metadataSize,
                           encodeBlockMetadata, 0, BLOCK_CONTEXT,
                           handleBlockCallback, &context->id);
  if (result == UDS_SUCCESS) {
    notifyBlockContextOpened(session, *context);
  }
  return result;
}
#endif /* NAMESPACES */

/**********************************************************************/
int udsCloseBlockContext(UdsBlockContext context)
{
  int result = closeContext(context.id, BLOCK_CONTEXT);
  if (result == UDS_SUCCESS) {
    notifyBlockContextClosed(context);
  }
  return result;
}

/**********************************************************************/
int udsFlushBlockContext(UdsBlockContext context)
{
  return flushContext(context.id, BLOCK_CONTEXT);
}

/**********************************************************************/
int udsStartChunkOperation(UdsRequest *request)
{
  if (request->callback == NULL) {
    return UDS_CALLBACK_REQUIRED;
  }
  switch (request->type) {
  case UDS_DELETE:
  case UDS_POST:
  case UDS_QUERY:
  case UDS_UPDATE:
    break;
  default:
    return UDS_INVALID_OPERATION_TYPE;
  }
  request->found = false;
  memset(request->private, 0, sizeof(request->private));
  return launchAllocatedClientRequest((Request *) request);
}

/**********************************************************************/
int udsUpdateBlockMapping(UdsBlockContext     context,
                          UdsCookie           cookie,
                          const UdsChunkName *blockName,
                          UdsBlockAddress     blockAddress)
{
  if (blockName == NULL) {
    return UDS_CHUNK_NAME_REQUIRED;
  }
  if (blockAddress == NULL) {
    return UDS_BLOCK_ADDRESS_REQUIRED;
  }
  return launchClientRequest(context.id, BLOCK_CONTEXT, UDS_UPDATE, false,
                             blockName, cookie, blockAddress, 0, NULL);
}

/**********************************************************************/
int udsDeleteBlockMapping(UdsBlockContext     context,
                          UdsCookie           cookie,
                          const UdsChunkName *blockName)
{
  if (blockName == NULL) {
    return UDS_CHUNK_NAME_REQUIRED;
  }
  return launchClientRequest(context.id, BLOCK_CONTEXT, UDS_DELETE, false,
                             blockName, cookie, NULL, 0, NULL);
}

/**********************************************************************/
int udsPostBlock(UdsBlockContext  context,
                 UdsCookie        cookie,
                 UdsBlockAddress  blockAddress,
                 size_t           blockLength,
                 const void      *data)
{
  if (blockAddress == NULL) {
    return UDS_BLOCK_ADDRESS_REQUIRED;
  }
  if (data == NULL) {
    return UDS_CHUNK_DATA_REQUIRED;
  }
  return launchClientRequest(context.id, BLOCK_CONTEXT, UDS_POST, false, NULL,
                             cookie, blockAddress, blockLength, data);
}

/**********************************************************************/
int udsPostBlockName(UdsBlockContext     context,
                     UdsCookie           cookie,
                     UdsBlockAddress     blockAddress,
                     const UdsChunkName *chunkName)
{
  if (blockAddress == NULL) {
    return UDS_BLOCK_ADDRESS_REQUIRED;
  }
  if (chunkName == NULL) {
    return UDS_CHUNK_NAME_REQUIRED;
  }
  return launchClientRequest(context.id, BLOCK_CONTEXT, UDS_POST, false,
                             chunkName, cookie, blockAddress, 0, NULL);
}

/**********************************************************************/
int udsQueryBlockName(UdsBlockContext     context,
                      UdsCookie           cookie,
                      UdsBlockAddress     blockAddress,
                      const UdsChunkName *blockName,
                      bool                update)
{
  if (blockName == NULL) {
    return UDS_CHUNK_NAME_REQUIRED;
  }
  return launchClientRequest(context.id, BLOCK_CONTEXT, UDS_QUERY, update,
                             blockName, cookie, blockAddress, 0, NULL);
}

/**********************************************************************/
int udsCheckBlock(UdsBlockContext  context,
                  UdsCookie        cookie,
                  UdsBlockAddress  blockAddress,
                  size_t           blockLength,
                  const void      *data,
                  bool             update)
{
  if (data == NULL) {
    return UDS_CHUNK_DATA_REQUIRED;
  }
  return launchClientRequest(context.id, BLOCK_CONTEXT, UDS_QUERY, update,
                             NULL, cookie, blockAddress, blockLength, data);
}

/**********************************************************************/
int udsRegisterDedupeBlockCallback(UdsBlockContext         context,
                                   UdsDedupeBlockCallback  cb,
                                   void                   *callbackArgument)
{
  IndexCallbackFunction callbackFunction;
  if (cb != NULL) {
    callbackFunction.blockCallback = cb;
  }
  return registerDedupeCallback(context.id, BLOCK_CONTEXT,
                                ((cb == NULL) ? NULL : &callbackFunction),
                                callbackArgument);
}

/**********************************************************************/
static void handleBlockCallback(Request *request)
{
  UdsBlockAddress duplicateAddress = NULL;
  if ((request->type != UDS_DELETE)) {
    duplicateAddress = &request->newMetadata.data;
  }

  UdsBlockAddress canonicalAddress = NULL;
  if (request->location != LOC_UNAVAILABLE) {
    canonicalAddress = &request->oldMetadata.data;
  }

  UdsContext *context = request->context;
  UdsBlockContext blockContext = { .id = context->id };
  UdsDedupeBlockCallback callback = context->callbackFunction.blockCallback;
  (*callback)(blockContext, request->type, request->status, request->cookie,
              duplicateAddress, canonicalAddress,
              &request->hash, request->dataLength, context->callbackArgument);
}

/**********************************************************************/
int udsSetBlockContextRequestQueueLimit(UdsBlockContext context,
                                        unsigned int    maxRequests)
{
  return setRequestQueueLimit(context.id, BLOCK_CONTEXT, maxRequests);
}

/**********************************************************************/
int udsSetBlockContextHashAlgorithm(UdsBlockContext  context,
                                    UdsHashAlgorithm algorithm)
{
  return setChunkNameAlgorithm(context.id, BLOCK_CONTEXT, algorithm);
}

/**********************************************************************/
int udsGetBlockContextConfiguration(UdsBlockContext   context,
                                    UdsConfiguration *conf)
{
  return getConfiguration(context.id, BLOCK_CONTEXT, conf);
}

/**********************************************************************/
int udsGetBlockContextIndexStats(UdsBlockContext  context,
                                 UdsIndexStats   *stats)
{
  return getContextIndexStats(context.id, BLOCK_CONTEXT, stats);
}

/**********************************************************************/
int udsGetBlockContextStats(UdsBlockContext  context,
                            UdsContextStats *stats)
{
  return getContextStats(context.id, BLOCK_CONTEXT, stats);
}

/**********************************************************************/
int udsResetBlockContextStats(UdsBlockContext context)
{
  return resetStats(context.id, BLOCK_CONTEXT);
}
