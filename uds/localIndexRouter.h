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
 * $Id: //eng/uds-releases/flanders-rhel7.5/src/uds/localIndexRouter.h#1 $
 */

#ifndef LOCAL_INDEX_ROUTER_H
#define LOCAL_INDEX_ROUTER_H

#include "config.h"
#include "featureDefs.h"
#include "indexLayout.h"
#include "indexRouter.h"
#include "loadType.h"
#include "request.h"

/**
 * LocalIndexRouter is used to distribute requests to and manage one or more
 * equal-sized Albireo index volumes on the local host. It used by the
 * embedded UDS library configuration and by albserver.
 *
 * The LocalIndexRouter implementation is completely private, not even having
 * a public opaque typedef. IndexRouter serves as a pseudo-object base class,
 * containing the function hooks used for almost all operations on the router.
 * See indexRouter.h for details on the operations supported by the router.
 **/

/**
 * Construct and initialize a LocalIndexRouter instance.
 *
 * @param path       the path where index-related files are stored
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

#ifdef HISTOGRAMS
/**
 * Start doing a histogram of the service time, and arrange for it to be
 * plotted at the program termination.
 *
 * @param name  Base name of the histogram file
 **/
void doServiceHistogram(const char *name);
#endif /* HISTOGRAMS */

#endif /* LOCAL_INDEX_ROUTER_H */
