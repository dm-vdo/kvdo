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
 * $Id: //eng/uds-releases/krusty-rhel9.0-beta/src/uds/index.h#1 $
 */

#ifndef INDEX_H
#define INDEX_H

#include "chapterWriter.h"
#include "indexLayout.h"
#include "indexSession.h"
#include "indexZone.h"
#include "loadType.h"
#include "masterIndexOps.h"
#include "request.h"
#include "volume.h"


/**
 * Index checkpoint state private to indexCheckpoint.c.
 **/
struct index_checkpoint;

/**
 * Callback after a query, update or remove request completes and fills in
 * select fields in the request: status for all requests, oldMetadata and
 * hashExists for query and update requests.
 *
 * @param request  request object
 **/
typedef void (*index_callback_t)(struct uds_request *request);

struct uds_index {
	bool has_saved_open_chapter;
	bool need_to_save;
	enum load_type loaded_type;
	struct index_load_context *load_context;
	struct index_layout *layout;
	struct index_state *state;
	struct volume_index *volume_index;
	struct volume *volume;
	unsigned int zone_count;
	struct index_zone **zones;

	/*
	 * ATTENTION!!!
	 * The meaning of the next two fields has changed.
	 *
	 * They now represent the oldest and newest chapters only at load time,
	 * and when the index is quiescent. At other times, they may lag
	 * individual zones' views of the index depending upon the progress
	 * made by the chapter writer.
	 */
	uint64_t oldest_virtual_chapter;
	uint64_t newest_virtual_chapter;

	uint64_t last_checkpoint;
	uint64_t prev_checkpoint;
	struct chapter_writer *chapter_writer;

	// checkpoint state used by indexCheckpoint.c
	struct index_checkpoint *checkpoint;

	index_callback_t callback;
	struct uds_request_queue *triage_queue;
	struct uds_request_queue *zone_queues[];
};

/**
 * Construct a new index from the given configuration.
 *
 * @param layout	The index layout
 * @param config	The configuration to use
 * @param user_params	The index session parameters.  If NULL, the default
 *			session parameters will be used.
 * @param load_type	How to create the index:  it can be create only, allow
 *			loading from files, and allow rebuilding from the
 *			volume
 * @param load_context	The load context to use
 * @param callback      the function to invoke when a request completes
 * @param new_index	A pointer to hold a pointer to the new index
 *
 * @return	   UDS_SUCCESS or an error code
 **/
int __must_check make_index(struct index_layout *layout,
			    const struct configuration *config,
			    const struct uds_parameters *user_params,
			    enum load_type load_type,
			    struct index_load_context *load_context,
			    index_callback_t callback,
			    struct uds_index **new_index);

/**
 * Construct a new index from the given configuration.
 *
 * @param layout       The index layout to use
 * @param config       The configuration to use
 * @param user_params  The index session parameters.  If NULL, the default
 *                     session parameters will be used.
 * @param zone_count   The number of zones for this index to use
 * @param new_index    A pointer to hold a pointer to the new index
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check allocate_index(struct index_layout *layout,
				const struct configuration *config,
				const struct uds_parameters *user_params,
				unsigned int zone_count,
				struct uds_index **new_index);

/**
 * Save an index.
 *
 * Before saving an index and while saving an index, the caller must ensure
 * that there are no index requests in progress.
 *
 * Some users follow save_index immediately with a free_index.	But some tests
 * use index_layout to modify the saved index.	The index will then have
 * some cached information that does not reflect these updates.
 *
 * @param index	  The index to save
 *
 * @return	  UDS_SUCCESS if successful
 **/
int __must_check save_index(struct uds_index *index);

/**
 * Clean up the index and its memory.
 *
 * @param index	  The index to destroy.
 **/
void free_index(struct uds_index *index);

/**
 * Perform the index operation specified by the type field of a UDS request.
 *
 * For UDS API requests, this searches the index for the chunk name in the
 * request. If the chunk name is already present in the index, the location
 * field of the request will be set to the uds_index_region where it was
 * found. If the action is not DELETE, the old_metadata field of the request
 * will also be filled in with the prior metadata for the name.
 *
 * If the API request type is:
 *
 *   UDS_POST, a record will be added to the open chapter with the metadata
 *     in the request for new records, and the existing metadata for existing
 *     records.
 *
 *   UDS_UPDATE, a record will be added to the open chapter with the metadata
 *     in the request.
 *
 *   UDS_QUERY, if the update flag is set in the request, any record found
 *     will be moved to the open chapter. In all other cases the contents of
 *     the index will remain unchanged.
 *
 *   UDS_DELETE, any entry with the name will removed from the index.
 *
 * @param index	      The index
 * @param request     The originating request
 *
 * @return UDS_SUCCESS, UDS_QUEUED, or an error code
 **/
int __must_check dispatch_index_request(struct uds_index *index,
					struct uds_request *request);

/**
 * Internal helper to prepare the index for saving.
 *
 * @param index		       the index
 * @param checkpoint	       whether the save is a checkpoint
 * @param open_chapter_number  the virtual chapter number of the open chapter
 **/
void begin_save(struct uds_index *index,
		bool checkpoint,
		uint64_t open_chapter_number);

/**
 * Replay the volume file to repopulate the volume index.
 *
 * @param index		The index
 * @param from_vcn	The virtual chapter to start replaying
 *
 * @return		UDS_SUCCESS if successful
 **/
int __must_check replay_volume(struct uds_index *index, uint64_t from_vcn);

/**
 * Gather statistics from the volume index, volume, and cache.
 *
 * @param index	    The index
 * @param counters  the statistic counters for the index
 **/
void get_index_stats(struct uds_index *index,
		     struct uds_index_stats *counters);

/**
 * Advance the newest virtual chapter. If this will overwrite the oldest
 * virtual chapter, advance that also.
 *
 * @param index The index to advance
 **/
void advance_active_chapters(struct uds_index *index);

/**
 * Select and return the request queue responsible for executing the next
 * index stage of a request, updating the request with any associated state
 * (such as the zone number).
 *
 * @param index       The index.
 * @param request     The request destined for the queue.
 * @param next_stage  The next request stage.
 *
 * @return the next index stage queue (the triage queue or the zone queue)
 **/
struct uds_request_queue *select_index_queue(struct uds_index *index,
					     struct uds_request *request,
					     enum request_stage next_stage);

/**
 * Wait for the index to finish all operations that access a local storage
 * device.
 *
 * @param index  The index
 **/
static INLINE void wait_for_idle_index(struct uds_index *index)
{
	wait_for_idle_chapter_writer(index->chapter_writer);
}

#endif /* INDEX_H */
