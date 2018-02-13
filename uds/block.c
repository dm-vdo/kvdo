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
 * $Id: //eng/uds-releases/flanders/src/uds/block.c#4 $
 */

#include "uds-block.h"

#include "context.h"
#include "featureDefs.h"
#include "request.h"
#include "uds-error.h"

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
  return openContext(session, namespace, metadataSize, &context->id);
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
  return openContext(session, metadataSize, &context->id);
}
#endif /* NAMESPACES */

/**********************************************************************/
int udsCloseBlockContext(UdsBlockContext context)
{
  return closeContext(context.id);
}

/**********************************************************************/
int udsFlushBlockContext(UdsBlockContext context)
{
  return flushContext(context.id);
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
  return launchClientRequest(context.id, UDS_UPDATE, false, blockName, cookie,
                             blockAddress, 0, NULL);
}

/**********************************************************************/
int udsDeleteBlockMapping(UdsBlockContext     context,
                          UdsCookie           cookie,
                          const UdsChunkName *blockName)
{
  if (blockName == NULL) {
    return UDS_CHUNK_NAME_REQUIRED;
  }
  return launchClientRequest(context.id, UDS_DELETE, false, blockName, cookie,
                             NULL, 0, NULL);
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
  return launchClientRequest(context.id, UDS_POST, false, NULL, cookie,
                             blockAddress, blockLength, data);
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
  return launchClientRequest(context.id, UDS_POST, false, chunkName, cookie,
                             blockAddress, 0, NULL);
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
  return launchClientRequest(context.id, UDS_QUERY, update, blockName, cookie,
                             blockAddress, 0, NULL);
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
  return launchClientRequest(context.id, UDS_QUERY, update, NULL, cookie,
                             blockAddress, blockLength, data);
}

/**********************************************************************/
int udsRegisterDedupeBlockCallback(UdsBlockContext         context,
                                   UdsDedupeBlockCallback  callbackFunction,
                                   void                   *callbackArgument)
{
  return registerDedupeCallback(context.id, callbackFunction,
                                callbackArgument);
}

/**********************************************************************/
int udsSetBlockContextRequestQueueLimit(UdsBlockContext context,
                                        unsigned int    maxRequests)
{
  return setRequestQueueLimit(context.id, maxRequests);
}

/**********************************************************************/
int udsSetBlockContextHashAlgorithm(UdsBlockContext  context,
                                    UdsHashAlgorithm algorithm)
{
  return setChunkNameAlgorithm(context.id, algorithm);
}

/**********************************************************************/
int udsGetBlockContextConfiguration(UdsBlockContext   context,
                                    UdsConfiguration *conf)
{
  return getConfiguration(context.id, conf);
}

/**********************************************************************/
int udsGetBlockContextIndexStats(UdsBlockContext  context,
                                 UdsIndexStats   *stats)
{
  return getContextIndexStats(context.id, stats);
}

/**********************************************************************/
int udsGetBlockContextStats(UdsBlockContext context, UdsContextStats *stats)
{
  return getContextStats(context.id, stats);
}

/**********************************************************************/
int udsResetBlockContextStats(UdsBlockContext context)
{
  return resetStats(context.id);
}
