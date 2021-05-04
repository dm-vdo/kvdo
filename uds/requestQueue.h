/*
 * Copyright Red Hat
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
 * $Id: //eng/uds-releases/krusty/src/uds/requestQueue.h#5 $
 */

#ifndef REQUEST_QUEUE_H
#define REQUEST_QUEUE_H

#include "compiler.h"
#include "opaqueTypes.h"
#include "typeDefs.h"

/* void return value because this function will process its own errors */
typedef void request_queue_processor_t(Request *);

/**
 * Allocate a new request processing queue and start a worker thread to
 * consume and service requests in the queue.
 *
 * @param queue_name   the name of the queue and the worker thread
 * @param process_one  the function the worker will invoke on each request
 * @param queue_ptr    a pointer to receive the new queue
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_request_queue(const char *queue_name,
				    request_queue_processor_t *process_one,
				    RequestQueue **queue_ptr);

/**
 * Add a request to the end of the queue for processing by the worker thread.
 * If the requeued flag is set on the request, it will be processed before
 * any non-requeued requests under most circumstances.
 *
 * @param queue    the request queue that should process the request
 * @param request  the request to be processed on the queue's worker thread
 **/
void request_queue_enqueue(RequestQueue *queue, Request *request);

/**
 * Shut down the request queue worker thread, then destroy and free the queue.
 *
 * @param queue  the queue to shut down and free
 **/
void request_queue_finish(RequestQueue *queue);

#endif /* REQUEST_QUEUE_H */
