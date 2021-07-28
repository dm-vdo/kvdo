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
 * $Id: //eng/uds-releases/krusty/src/uds/request.h#17 $
 */

#ifndef REQUEST_H
#define REQUEST_H

#include "cacheCounters.h"
#include "common.h"
#include "compiler.h"
#include "threads.h"
#include "timeUtils.h"
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
 * @param router   The index router responsible for handling the message
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check launch_zone_message(struct uds_zone_message message,
				     unsigned int zone,
				     struct index_router *router);

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
 * Compute the cache_probe_type value reflecting the request and page type.
 *
 * @param request      The request being processed, or NULL
 * @param is_index_page  Whether the cache probe will be for an index page
 *
 * @return the cache probe type enumeration
 **/
static INLINE enum cache_probe_type
cache_probe_type(struct uds_request *request, bool is_index_page)
{
	if ((request != NULL) && request->requeued) {
		return is_index_page ? CACHE_PROBE_INDEX_RETRY :
				       CACHE_PROBE_RECORD_RETRY;
	} else {
		return is_index_page ? CACHE_PROBE_INDEX_FIRST :
				       CACHE_PROBE_RECORD_FIRST;
	}
}
#endif /* REQUEST_H */
