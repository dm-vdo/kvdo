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
 * $Id: //eng/uds-releases/gloria/src/uds/localIndexRouterPrivate.h#1 $
 */

#ifndef LOCAL_INDEX_ROUTER_PRIVATE_H
#define LOCAL_INDEX_ROUTER_PRIVATE_H

#include "compiler.h"
#include "index.h"

/**
 * XXX document LocalIndexRouter structure and fields
 **/
typedef struct localIndexRouter {
  IndexRouter    header;
  unsigned int   zoneCount;
  RequestQueue **zoneQueues;
  RequestQueue  *triageQueue;
  Index         *index;
} LocalIndexRouter;

/**
 * Convert an IndexRouter pointer to a LocalIndexRouter pointer.
 **/
static INLINE LocalIndexRouter *asLocalIndexRouter(IndexRouter *header)
{
  return container_of(header, LocalIndexRouter, header);
}

#endif /* LOCAL_INDEX_ROUTER_PRIVATE_H */
