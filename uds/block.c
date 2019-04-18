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
 * $Id: //eng/uds-releases/homer/src/uds/block.c#1 $
 */

#include "uds-block.h"

#include "context.h"
#include "featureDefs.h"
#include "request.h"
#include "uds-error.h"

/**********************************************************************/
int udsOpenBlockContext(UdsIndexSession  session,
                        unsigned int     metadataSize __attribute__((unused)),
                        UdsBlockContext *context)
{
  // XXX the metadataSize argument is unused and can be deleted
  if (context == NULL) {
    return UDS_CONTEXT_PTR_REQUIRED;
  }
  return openContext(session, &context->id);
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
