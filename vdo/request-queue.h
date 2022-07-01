/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef REQUEST_QUEUE_H
#define REQUEST_QUEUE_H

#include "compiler.h"
#include "type-defs.h"
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
