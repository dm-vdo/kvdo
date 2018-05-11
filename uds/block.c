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
 * $Id: //eng/uds-releases/flanders/src/uds/block.c#8 $
 */

#include "uds-block.h"

#include "context.h"
#include "featureDefs.h"
#include "request.h"
#include "uds-error.h"

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
int udsPostBlockName(UdsBlockContext     context,
                     UdsCookie           cookie,
                     UdsBlockAddress     blockAddress,
                     const UdsChunkName *chunkName)
{
  if (blockAddress == NULL) {
    return UDS_BLOCK_ADDRESS_REQUIRED;
  }
  return launchClientRequest(context.id, UDS_POST, false, chunkName, cookie,
                             blockAddress);
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
