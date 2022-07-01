/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef REQUEST_H
#define REQUEST_H

#include "common.h"
#include "compiler.h"
#include "uds-threads.h"
#include "time-utils.h"
#include "uds.h"

/**
 * Abstract request pipeline stages, which can also be viewed as stages in the
 * life-cycle of a request.
 **/
enum request_stage {
	STAGE_TRIAGE,
	STAGE_INDEX,
	STAGE_CALLBACK,
	STAGE_MESSAGE,
};

typedef void (*request_restarter_t)(struct uds_request *);

/**
 * Make an asynchronous control message for an index zone and enqueue it for
 * processing.
 *
 * @param message  The message to send
 * @param zone     The zone number of the zone to receive the message
 * @param index    The index responsible for handling the message
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check launch_zone_message(struct uds_zone_message message,
				     unsigned int zone,
				     struct uds_index *index);

/**
 * Enqueue a request for the next stage of the pipeline. If there is more than
 * one possible queue for a stage, this function uses the request to decide
 * which queue should handle it.
 *
 * @param request       The request to enqueue
 * @param next_stage    The next stage of the pipeline to process the request
 **/
void enqueue_request(struct uds_request *request,
		     enum request_stage next_stage);

/**
 * A method to restart delayed requests.
 *
 * @param request    The request to restart
 **/
void restart_request(struct uds_request *request);

/**
 * Set the function pointer which is used to restart requests.
 * This is needed by albserver code and is used as a test hook by the unit
 * tests.
 *
 * @param restarter   The function to call to restart requests.
 **/
void set_request_restarter(request_restarter_t restarter);

/**
 * Enter the callback stage of processing for a request, notifying the waiting
 * thread if the request is synchronous, freeing the request if it is an
 * asynchronous control message, or placing it on the callback queue if it is
 * an asynchronous client request.
 *
 * @param request  the request which has completed execution
 **/
void enter_callback_stage(struct uds_request *request);

/**
 * Update the context statistics to reflect the successful completion of a
 * client request.
 *
 * @param request  a client request that has successfully completed execution
 **/
void update_request_context_stats(struct uds_request *request);

/**
 * Set the location and found flag together so they remain in sync.
 *
 * @param request       The request being processed
 * @param new_location  The new location
 **/
static INLINE void set_request_location(struct uds_request *request,
					enum uds_index_region new_location)
{
  request->location = new_location;
  request->found = ((new_location == UDS_LOCATION_IN_OPEN_CHAPTER) ||
		    (new_location == UDS_LOCATION_IN_DENSE) ||
		    (new_location == UDS_LOCATION_IN_SPARSE));
}

#endif /* REQUEST_H */
