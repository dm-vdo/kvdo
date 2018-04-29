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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/kvdoFlush.h#1 $
 */

#ifndef KVDO_FLUSH_H
#define KVDO_FLUSH_H

#include "flush.h"

#include "kernelLayer.h"

/**
 * Create a KVDOFlush.
 *
 * @param flushPtr  A pointer to hold the new flush
 **/
int makeKVDOFlush(KVDOFlush **flushPtr);

/**
 * Answer the question as to whether VDO should be processing REQ_FLUSH
 * requests or not.
 *
 * @param layer    The layer
 *
 * @return true if VDO should process empty flush requests, or false if
 *         they should just be forwarded to our storage device.
 **/
bool shouldProcessFlush(KernelLayer *layer);

/**
 * Function called to start processing a flush request. It is called when we
 * receive an empty flush bio from the block layer, and before acknowledging a
 * non-empty bio with the FUA flag set.
 *
 * @param layer  The physical layer
 * @param bio    The bio containing an empty flush request
 **/
void launchKVDOFlush(KernelLayer *layer, BIO *bio);

/**
 * Function called from base VDO to complete and free a flush request.
 *
 * @param kfp  Pointer to the flush request
 **/
void kvdoCompleteFlush(VDOFlush **kfp);

/**
 * Issue a flush request and wait for it to complete.
 *
 * @param layer The kernel layer
 *
 * @return VDO_SUCCESS or an error
 */
int synchronousFlush(KernelLayer *layer);

#endif /* KVDO_FLUSH_H */
