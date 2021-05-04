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
 * $Id: //eng/uds-releases/krusty/src/uds/indexRouter.h#15 $
 */

#ifndef INDEX_ROUTER_H
#define INDEX_ROUTER_H

#include "compiler.h"
#include "index.h"
#include "indexSession.h"
#include "request.h"

/**
 * Callback after a query, update or remove request completes and fills in
 * select fields in the request: status for all requests, oldMetadata and
 * hashExists for query and update requests.
 *
 * @param request     request object.
 **/
typedef void (*index_router_callback_t)(Request *request);

struct index_router {
	index_router_callback_t callback;
	unsigned int zone_count;
	bool need_to_save;
	struct index *index;
	RequestQueue *triage_queue;
	RequestQueue *zone_queues[];
};

/**
 * Construct and initialize an index_router instance.
 *
 * @param layout        the index_layout that describes the stored index
 * @param config        the configuration to use
 * @param user_params   the index session parameters.  If NULL, the default
 *                      session parameters will be used.
 * @param load_type     selects whether to create, load, or rebuild the index
 * @param load_context  the index load context to use
 * @param callback      the function to invoke when a request completes or fails
 * @param router_ptr    a pointer in which to store the new router
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_index_router(struct index_layout *layout,
				   const struct configuration *config,
				   const struct uds_parameters *user_params,
				   enum load_type load_type,
				   struct index_load_context *load_context,
				   index_router_callback_t callback,
				   struct index_router **router_ptr);

/**
 * Executes the index operation for a UDS request and calls the callback upon
 * completion.
 *
 * @param router      The index router.
 * @param request     A pointer to the Request to process.
 **/
void execute_index_router_request(struct index_router *router,
				  Request *request);

/**
 * Save the index router state to persistent storage.
 *
 * It is the responsibility of the caller to ensure that there are no other
 * uses of the index during a call to this method.  It is necessary that there
 * be no index requests from any block context nor any other attempt to save
 * the index until after a call to save_index_router returns.
 *
 * @param router  the index router to save
 *
 * @return UDS_SUCCESS if successful.
 **/
int __must_check save_index_router(struct index_router *router);

/**
 * Destroy the index router and free its memory.
 *
 * @param router  the index router to destroy (may be NULL)
 **/
void free_index_router(struct index_router *router);

/**
 * Select and return the request queue responsible for executing the next
 * index stage of a request, updating the request with any associated state
 * (such as the zone number for UDS requests on a local index).
 *
 * @param router      The index router.
 * @param request     The Request destined for the queue.
 * @param next_stage  The next request stage (STAGE_TRIAGE or STAGE_INDEX).
 *
 * @return the next index stage queue (the local triage queue, local zone
 *         queue, or remote RPC send queue)
 **/
RequestQueue *select_index_router_queue(struct index_router *router,
					Request *request,
					enum request_stage next_stage);

/**
 * Wait for the index router to finish all operations that access a local
 * storage device.
 *
 * @param router    The index router.
 **/
static INLINE void wait_for_idle_index_router(struct index_router *router)
{
	wait_for_idle_chapter_writer(router->index->chapter_writer);
}

#endif /* INDEX_ROUTER_H */
