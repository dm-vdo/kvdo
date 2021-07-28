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
 * $Id: //eng/uds-releases/krusty/src/uds/requestQueue.h#8 $
 */

#ifndef REQUEST_QUEUE_H
#define REQUEST_QUEUE_H

#include "compiler.h"
#include "typeDefs.h"
#include "uds.h"

struct uds_request_queue;

/* void return value because this function will process its own errors */
typedef void uds_request_queue_processor_t(struct uds_request *);

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
int __must_check
make_uds_request_queue(const char *queue_name,
		       uds_request_queue_processor_t *process_one,
		       struct uds_request_queue **queue_ptr);

/**
 * Add a request to the end of the queue for processing by the worker thread.
 * If the requeued flag is set on the request, it will be processed before
 * any non-requeued requests under most circumstances.
 *
 * @param queue    the request queue that should process the request
 * @param request  the request to be processed on the queue's worker thread
 **/
void uds_request_queue_enqueue(struct uds_request_queue *queue,
			       struct uds_request *request);

/**
 * Shut down the request queue worker thread, then destroy and free the queue.
 *
 * @param queue  the queue to shut down and free
 **/
void uds_request_queue_finish(struct uds_request_queue *queue);

#endif /* REQUEST_QUEUE_H */
