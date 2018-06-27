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
 * $Id: //eng/uds-releases/gloria/src/uds/localIndexRouter.h#2 $
 */

#ifndef LOCAL_INDEX_ROUTER_H
#define LOCAL_INDEX_ROUTER_H

#include "compiler.h"
#include "index.h"
#include "indexRouter.h"

/**
 * LocalIndexRouter is used to distribute requests to and manage one or more
 * equal-sized Albireo index volumes on the local host.  It used by the
 * embedded UDS library configuration and by albserver.
 *
 * IndexRouter serves as a pseudo-object base class, containing the function
 * hooks used for almost all operations on the router.  See indexRouter.h for
 * details on the operations supported by the router.
 **/

typedef struct localIndexRouter {
  IndexRouter   header;
  unsigned int  zoneCount;
  Index        *index;
  RequestQueue *triageQueue;
  RequestQueue *zoneQueues[];
} LocalIndexRouter;

/**
 * Convert an IndexRouter pointer to a LocalIndexRouter pointer.
 **/
static INLINE LocalIndexRouter *asLocalIndexRouter(IndexRouter *header)
{
  return container_of(header, LocalIndexRouter, header);
}

/**
 * Construct and initialize a LocalIndexRouter instance.
 *
 * @param layout     the IndexLayout that describes the stored index
 * @param config     the configuration to use
 * @param loadType   selects whether to create, load, or rebuild the index
 * @param callback   the function to invoke when a request completes or fails
 * @param newRouter  a pointer in which to store the new router
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeLocalIndexRouter(IndexLayout          *layout,
                         const Configuration  *config,
                         LoadType              loadType,
                         IndexRouterCallback   callback,
                         IndexRouter         **newRouter)
  __attribute__((warn_unused_result));

#endif /* LOCAL_INDEX_ROUTER_H */
