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
 * $Id: //eng/uds-releases/flanders/src/uds/requestLimit.h#2 $
 */

#ifndef REQUEST_LIMIT_H
#define REQUEST_LIMIT_H

#include "opaqueTypes.h"
#include "typeDefs.h"

/**
 * RequestLimit is used to restrict the number of client requests allocated at
 * one time. The asynchronous client interface would make it easy for a single
 * thread to issue and allocate millions of outstanding requests, consuming
 * too much memory and other resources. RequestLimit dispenses permits, and
 * each permit is the right for the client thread to allocate and dispatch a
 * single request. Limits are enforced separately for each client context.
 *
 * A RequestLimit is functionally equivalent to a counting semaphore, safely
 * dispensing to multiple threads a maximum number of permits at any one time,
 * and accepting the return of permits from threads that did not acquire them.
 * If no permits are available when one is requested, the thread blocks until
 * one becomes available.
 *
 * Unlike a semaphore, the implementation optimizes for the asymmetry of the
 * request pipeline, where permits are always acquired on a client thread and
 * always returned on a pipeline thread (typically the callback queue worker
 * thread).
 *
 * To reduce the amount of context switching, particularly when the client
 * threads run faster than the request pipeline, the implementation also
 * attempts to "batch" the released permits. Batching means that, if the
 * client finally wakes up and gets one permit, it is likely to be able to get
 * several others before blocking again.
 **/

// RequestLimit is an opaque structure type declared in opaqueTypes.h and the
// structure is private and declared in requestLimit.c.

/**
 * Allocate and initialize a new request limit with the specified total number
 * of permits.
 *
 * @param [in]  permits   the total number of permits that may be borrowed
 * @param [out] limitPtr  a pointer to receive the new request limit
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeRequestLimit(uint32_t permits, RequestLimit **limitPtr)
  __attribute__((warn_unused_result));

/**
 * Free a request limit. Waits for all permits to be returned before
 * destroying the limit itself.
 *
 * @param limit  the limit to destroy
 **/
void freeRequestLimit(RequestLimit *limit);

/**
 * Get the total number of request permits that may be borrowed.
 *
 * @param limit  the request limit
 *
 * @return the total number of request permits
 **/
uint32_t getRequestPermitLimit(RequestLimit *limit);

/**
 * Set the total number of request permits that may be borrowed.
 *
 * If the number of permits is being reduced, this will block and wait for
 * permits to be returned if, at the time of the call, fewer permits are
 * currently available than the change in the total number of permits.
 *
 * @param limit    the request limit
 * @param permits  the new total number of permits that may be borrowed
 **/
void setRequestPermitLimit(RequestLimit *limit, uint32_t permits);

/**
 * Borrow a request permit, giving the borrower the right to allocate a
 * request.
 *
 * This will block and wait for a permit to be returned if none are currently
 * ready for lending. (Permits may have been returned but might not yet be
 * ready to lend because of batching.) Each permit borrowed must eventually be
 * returned--callers promise not to lose or leak permits. To reduce the total
 * number of permits, use setPermitLimit() to decrease the limit.
 *
 * @param limit  the request limit
 **/
void borrowRequestPermit(RequestLimit *limit);

/**
 * Return request permits to the request limit, making them available to be
 * borrowed again. The requests that were allocated using the permits should
 * have already been freed when this is called.
 *
 * This function allows multiple permits to be returned together to reduce
 * contention and to provide additional batching. Each permit returned must
 * actually have been borrowed--callers promise never to return a permit more
 * than once each time one is borrowed, nor to manufacture additional permits.
 * To add permits to the limit, use setPermitLimit() to increase the limit.
 *
 * @param limit    the request limit
 * @param permits  the number of borrowed permits being returned to the limit
 **/
void returnRequestPermits(RequestLimit *limit, uint32_t permits);

#endif /* REQUEST_LIMIT_H */
