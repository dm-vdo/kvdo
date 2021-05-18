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
 * $Id: //eng/uds-releases/krusty/src/uds/index.c#41 $
 */


#include "index.h"

#include "hashUtils.h"
#include "indexCheckpoint.h"
#include "indexInternals.h"
#include "logger.h"

static const uint64_t NO_LAST_CHECKPOINT = UINT_MAX;


/**
 * Replay an index which was loaded from a checkpoint.
 *
 * @param index			   The index to replay
 * @param last_checkpoint_chapter  The number of the chapter where the
 *				   last checkpoint was made
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int replay_index_from_checkpoint(struct index *index,
					uint64_t last_checkpoint_chapter)
{
	// Find the volume chapter boundaries
	unsigned int chapters_per_volume;
	int result;
	uint64_t lowest_vcn, highest_vcn, first_replay_chapter;
	bool is_empty = false;
	enum index_lookup_mode old_lookup_mode = index->volume->lookup_mode;
	index->volume->lookup_mode = LOOKUP_FOR_REBUILD;
	result = find_volume_chapter_boundaries(index->volume,
						    &lowest_vcn, &highest_vcn,
						    &is_empty);
	index->volume->lookup_mode = old_lookup_mode;
	if (result != UDS_SUCCESS) {
		return log_fatal_strerror(result,
					  "cannot replay index: unknown volume chapter boundaries");
	}
	if (lowest_vcn > highest_vcn) {
		uds_log_fatal("cannot replay index: no valid chapters exist");
		return UDS_CORRUPT_COMPONENT;
	}

	if (is_empty) {
		// The volume is empty, so the index should also be empty
		if (index->newest_virtual_chapter != 0) {
			uds_log_fatal("cannot replay index from empty volume");
			return UDS_CORRUPT_COMPONENT;
		}
		return UDS_SUCCESS;
	}

	chapters_per_volume = index->volume->geometry->chapters_per_volume;
	index->oldest_virtual_chapter = lowest_vcn;
	index->newest_virtual_chapter = highest_vcn + 1;
	if (index->newest_virtual_chapter ==
	    lowest_vcn + chapters_per_volume) {
		// skip the chapter shadowed by the open chapter
		index->oldest_virtual_chapter++;
	}

	first_replay_chapter = last_checkpoint_chapter;
	if (first_replay_chapter < index->oldest_virtual_chapter) {
		first_replay_chapter = index->oldest_virtual_chapter;
	}
	return replay_volume(index, first_replay_chapter);
}

/**********************************************************************/
static int load_index(struct index *index, bool allow_replay)
{
	uint64_t last_checkpoint_chapter;
	unsigned int i;
	bool replay_required = false;

	int result = load_index_state(index->state, &replay_required);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (replay_required && !allow_replay) {
		return log_error_strerror(UDS_INDEX_NOT_SAVED_CLEANLY,
					  "index not saved cleanly: open chapter missing");
	}

	last_checkpoint_chapter =
		((index->last_checkpoint != NO_LAST_CHECKPOINT) ?
			 index->last_checkpoint :
			 0);

	log_info("loaded index from chapter %llu through chapter %llu",
		 index->oldest_virtual_chapter,
		 last_checkpoint_chapter);

	if (replay_required) {
		result = replay_index_from_checkpoint(index,
						      last_checkpoint_chapter);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	for (i = 0; i < index->zone_count; i++) {
		set_active_chapters(index->zones[i]);
	}

	index->loaded_type = replay_required ? LOAD_REPLAY : LOAD_LOAD;
	return UDS_SUCCESS;
}

/**********************************************************************/
static int rebuild_index(struct index *index)
{
	// Find the volume chapter boundaries
	int result;
	unsigned int i;
	uint64_t lowest_vcn, highest_vcn;
	bool is_empty = false;
	enum index_lookup_mode old_lookup_mode = index->volume->lookup_mode;
	index->volume->lookup_mode = LOOKUP_FOR_REBUILD;
	result = find_volume_chapter_boundaries(index->volume, &lowest_vcn,
						    &highest_vcn, &is_empty);
	index->volume->lookup_mode = old_lookup_mode;
	if (result != UDS_SUCCESS) {
		return log_fatal_strerror(result,
					  "cannot rebuild index: unknown volume chapter boundaries");
	}
	if (lowest_vcn > highest_vcn) {
		uds_log_fatal("cannot rebuild index: no valid chapters exist");
		return UDS_CORRUPT_COMPONENT;
	}

	if (is_empty) {
		index->newest_virtual_chapter =
			index->oldest_virtual_chapter = 0;
	} else {
		unsigned int num_chapters =
			index->volume->geometry->chapters_per_volume;
		index->newest_virtual_chapter = highest_vcn + 1;
		index->oldest_virtual_chapter = lowest_vcn;
		if (index->newest_virtual_chapter ==
		    (index->oldest_virtual_chapter + num_chapters)) {
			// skip the chapter shadowed by the open chapter
			index->oldest_virtual_chapter++;
		}
	}

	if ((index->newest_virtual_chapter - index->oldest_virtual_chapter) >
	    index->volume->geometry->chapters_per_volume) {
		return log_fatal_strerror(UDS_CORRUPT_COMPONENT,
					  "cannot rebuild index: volume chapter boundaries too large");
	}

	set_volume_index_open_chapter(index->volume_index, 0);
	if (is_empty) {
		index->loaded_type = LOAD_EMPTY;
		return UDS_SUCCESS;
	}

	result = replay_volume(index, index->oldest_virtual_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < index->zone_count; i++) {
		set_active_chapters(index->zones[i]);
	}

	index->loaded_type = LOAD_REBUILD;
	return UDS_SUCCESS;
}

/**********************************************************************/
int make_index(struct index_layout *layout,
	       const struct configuration *config,
	       const struct uds_parameters *user_params,
	       unsigned int zone_count,
	       enum load_type load_type,
	       struct index_load_context *load_context,
	       struct index **new_index)
{
	struct index *index;
	uint64_t nonce;
	int result = allocate_index(layout, config, user_params, zone_count,
				    load_type, &index);
	if (result != UDS_SUCCESS) {
		return log_error_strerror(result,
					  "could not allocate index");
	}

	index->load_context = load_context;

	nonce = get_volume_nonce(layout);
	result = make_volume_index(config, zone_count, nonce,
				   &index->volume_index);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return log_error_strerror(result,
					  "could not make volume index");
	}

	result = add_index_state_component(index->state, VOLUME_INDEX_INFO,
					    NULL, index->volume_index);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = add_index_state_component(index->state,
					   &INDEX_PAGE_MAP_INFO,
					   index->volume->index_page_map,
					   NULL);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = make_chapter_writer(index, get_index_version(layout),
				     &index->chapter_writer);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	if ((load_type == LOAD_LOAD) || (load_type == LOAD_REBUILD)) {
		if (!index->existed) {
			free_index(index);
			return UDS_NO_INDEX;
		}
		result = load_index(index, load_type == LOAD_REBUILD);
		switch (result) {
		case UDS_SUCCESS:
			break;
		case ENOMEM:
			// We should not try a rebuild for this error.
			log_error_strerror(result,
					   "index could not be loaded");
			break;
		default:
			log_error_strerror(result,
					   "index could not be loaded");
			if (load_type == LOAD_REBUILD) {
				result = rebuild_index(index);
				if (result != UDS_SUCCESS) {
					log_error_strerror(result,
							   "index could not be rebuilt");
				}
			}
			break;
		}
	} else {
		index->loaded_type = LOAD_CREATE;
		discard_index_state_data(index->state);
	}

	if (result != UDS_SUCCESS) {
		free_index(index);
		return log_unrecoverable(result, "fatal error in make_index");
	}

	if (index->load_context != NULL) {
		lock_mutex(&index->load_context->mutex);
		index->load_context->status = INDEX_READY;
		// If we get here, suspend is meaningless, but notify any
		// thread trying to suspend us so it doesn't hang.
		broadcast_cond(&index->load_context->cond);
		unlock_mutex(&index->load_context->mutex);
	}

	index->has_saved_open_chapter = index->loaded_type == LOAD_LOAD;
	*new_index = index;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_index(struct index *index)
{
	if (index == NULL) {
		return;
	}
	free_chapter_writer(index->chapter_writer);

	if (index->volume_index != NULL) {
		free_volume_index(index->volume_index);
	}
	release_index(index);
}

/**********************************************************************/
int save_index(struct index *index)
{
	int result;
	wait_for_idle_chapter_writer(index->chapter_writer);
	result = finish_checkpointing(index);
	if (result != UDS_SUCCESS) {
		log_info("save index failed");
		return result;
	}
	begin_save(index, false, index->newest_virtual_chapter);

	result = save_index_state(index->state);
	if (result != UDS_SUCCESS) {
		log_info("save index failed");
		index->last_checkpoint = index->prev_checkpoint;
	} else {
		index->has_saved_open_chapter = true;
		log_info("finished save (vcn %llu)",
			 index->last_checkpoint);
	}
	return result;
}

/**
 * Get the zone for a request.
 *
 * @param index The index
 * @param request The request
 *
 * @return The zone for the request
 **/
static struct index_zone *get_request_zone(struct index *index,
					   Request *request)
{
	return index->zones[request->zone_number];
}

/**
 * Search an index zone. This function is only correct for LRU.
 *
 * @param zone		    The index zone to query.
 * @param request	    The request originating the query.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int search_index_zone(struct index_zone *zone, Request *request)
{
	struct volume_index_record record;
	bool overflow_record, found = false;
	struct uds_chunk_data *metadata;
	uint64_t chapter;
	int result = get_volume_index_record(zone->index->volume_index,
					     &request->chunk_name, &record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (record.is_found) {
		result = get_record_from_zone(zone, request, &found,
					      record.virtual_chapter);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if (found) {
			request->location =
				compute_index_region(zone,
						     record.virtual_chapter);
		}
	}

	/*
	 * If a record has overflowed a chapter index in more than one chapter
	 * (or overflowed in one chapter and collided with an existing record),
	 * it will exist as a collision record in the volume index, but
	 * we won't *find it in the volume. This case needs special handling.
	 */
	overflow_record = (record.is_found && record.is_collision && !found);
	chapter = zone->newest_virtual_chapter;
	if (found || overflow_record) {
		if ((request->action == REQUEST_QUERY) &&
		    (!request->update || overflow_record)) {
			/* This is a query without update, or with nothing to
			 * update */
			return UDS_SUCCESS;
		}

		if (record.virtual_chapter != chapter) {
			/*
			 * Update the volume index to reference the new chapter
			 * for the block. If the record had been deleted or
			 * dropped from the chapter index, it will be back.
			 */
			result = set_volume_index_record_chapter(&record,
								 chapter);
		} else if (request->action != REQUEST_UPDATE) {
			/* The record is already in the open chapter, so we're
			 * done */
			return UDS_SUCCESS;
		}
	} else {
		// The record wasn't in the volume index, so check whether the
		// name is in a cached sparse chapter.
		if (!is_volume_index_sample(zone->index->volume_index,
					    &request->chunk_name) &&
		    is_sparse(zone->index->volume->geometry)) {
			// Passing UINT64_MAX triggers a search of the entire
			// sparse cache.
			result = search_sparse_cache_in_zone(zone, request,
							     UINT64_MAX,
							     &found);
			if (result != UDS_SUCCESS) {
				return result;
			}

			if (found) {
				request->location = LOC_IN_SPARSE;
			}
		}

		if (request->action == REQUEST_QUERY) {
			if (!found || !request->update) {
				// This is a query without update or for a new
				// record, so we're done.
				return UDS_SUCCESS;
			}
		}

		/*
		 * Add a new entry to the volume index referencing the open
		 * chapter. This needs to be done both for new records, and for
		 * records from cached sparse chapters.
		 */
		result = put_volume_index_record(&record, chapter);
	}

	if (result == UDS_OVERFLOW) {
		/*
		 * The volume index encountered a delta list overflow.	The
		 * condition was already logged. We will go on without adding
		 * the chunk to the open chapter.
		 */
		return UDS_SUCCESS;
	}

	if (result != UDS_SUCCESS) {
		return result;
	}

	if (!found || (request->action == REQUEST_UPDATE)) {
		// This is a new record or we're updating an existing record.
		metadata = &request->new_metadata;
	} else {
		// This is a duplicate, so move the record to the open chapter
		// (for LRU).
		metadata = &request->old_metadata;
	}
	return put_record_in_zone(zone, request, metadata);
}

/**********************************************************************/
static int remove_from_index_zone(struct index_zone *zone, Request *request)
{
	struct volume_index_record record;
	int result = get_volume_index_record(zone->index->volume_index,
					     &request->chunk_name, &record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (!record.is_found) {
		// The name does not exist in volume index, so there is nothing
		// to remove.
		return UDS_SUCCESS;
	}

	if (!record.is_collision) {
		// Non-collision records are hints, so resolve the name in the
		// chapter.
		bool found;
		int result = get_record_from_zone(zone, request, &found,
						  record.virtual_chapter);
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (!found) {
			// The name does not exist in the chapter, so there is
			// nothing to remove.
			return UDS_SUCCESS;
		}
	}

	request->location = compute_index_region(zone, record.virtual_chapter);

	/*
	 * Delete the volume index entry for the named record only. Note that a
	 * later search might later return stale advice if there is a colliding
	 * name in the same chapter, but it's a very rare case (1 in 2^21).
	 */
	result = remove_volume_index_record(&record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// If the record is in the open chapter, we must remove it or mark it
	// deleted to avoid trouble if the record is added again later.
	if (request->location == LOC_IN_OPEN_CHAPTER) {
		bool hash_exists = false;
		remove_from_open_chapter(zone->open_chapter,
					 &request->chunk_name,
					 &hash_exists);
		result = ASSERT(hash_exists,
				"removing record not found in open chapter");
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**
 * Simulate the creation of a sparse cache barrier message by the triage
 * queue, and the later execution of that message in an index zone.
 *
 * If the index receiving the request is multi-zone or dense, this function
 * does nothing. This simulation is an optimization for single-zone sparse
 * indexes. It also supports unit testing of indexes without routers and
 * queues.
 *
 * @param zone	   the index zone responsible for the index request
 * @param request  the index request about to be executed
 *
 * @return UDS_SUCCESS always
 **/
static int simulate_index_zone_barrier_message(struct index_zone *zone,
					       Request *request)
{
	struct barrier_message_data barrier;
	uint64_t sparse_virtual_chapter;
	// Do nothing unless this is a single-zone sparse index.
	if ((zone->index->zone_count > 1) ||
	    !is_sparse(zone->index->volume->geometry)) {
		return UDS_SUCCESS;
	}

	// Check if the index request is for a sampled name in a sparse
	// chapter.
	sparse_virtual_chapter = triage_index_request(zone->index, request);
	if (sparse_virtual_chapter == UINT64_MAX) {
		// Not indexed, not a hook, or in a chapter that is still
		// dense, which means there should be no change to the sparse
		// chapter index cache.
		return UDS_SUCCESS;
	}

	/*
	 * The triage queue would have generated and enqueued a barrier message
	 * preceding this request, which we simulate by directly invoking the
	 * execution hook for an equivalent message.
	 */
	barrier = (struct barrier_message_data) {
		.virtual_chapter = sparse_virtual_chapter
	};
	return execute_sparse_cache_barrier_message(zone, &barrier);
}

/**********************************************************************/
static int dispatch_index_zone_request(struct index_zone *zone,
				       Request *request)
{
	int result;
	if (!request->requeued) {
		// Single-zone sparse indexes don't have a triage queue to
		// generate cache barrier requests, so see if we need to
		// synthesize a barrier.
		int result =
			simulate_index_zone_barrier_message(zone, request);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	// Set the default location. It will be overwritten if we find the
	// chunk.
	request->location = LOC_UNAVAILABLE;

	switch (request->action) {
	case REQUEST_INDEX:
	case REQUEST_UPDATE:
	case REQUEST_QUERY:
		result = make_unrecoverable(search_index_zone(zone, request));
		break;

	case REQUEST_DELETE:
		result =
			make_unrecoverable(remove_from_index_zone(zone,
								  request));
		break;

	default:
		result = log_warning_strerror(UDS_INVALID_ARGUMENT,
					      "attempted to execute invalid action: %d",
					      request->action);
		break;
	}

	return result;
}

/**********************************************************************/
int dispatch_index_request(struct index *index, Request *request)
{
	return dispatch_index_zone_request(get_request_zone(index, request),
					   request);
}

/**********************************************************************/
static int rebuild_index_page_map(struct index *index, uint64_t vcn)
{
	struct geometry *geometry = index->volume->geometry;
	unsigned int chapter = map_to_physical_chapter(geometry, vcn);
	unsigned int expected_list_number = 0;
	unsigned int index_page_number;
	for (index_page_number = 0;
	     index_page_number < geometry->index_pages_per_chapter;
	     index_page_number++) {
		unsigned int lowest_delta_list, highest_delta_list;
		struct delta_index_page *chapter_index_page;
		int result = get_volume_page(index->volume,
					     chapter, index_page_number,
					     CACHE_PROBE_INDEX_FIRST, NULL,
					     &chapter_index_page);
		if (result != UDS_SUCCESS) {
			return log_error_strerror(result,
						  "failed to read index page %u in chapter %u",
						  index_page_number,
						  chapter);
		}
		lowest_delta_list = chapter_index_page->lowest_list_number;
		highest_delta_list = chapter_index_page->highest_list_number;
		if (lowest_delta_list != expected_list_number) {
			return log_error_strerror(UDS_CORRUPT_DATA,
						  "chapter %u index page %u is corrupt",
						  chapter,
						  index_page_number);
		}
		result = update_index_page_map(index->volume->index_page_map,
					       vcn,
					       chapter,
					       index_page_number,
					       highest_delta_list);
		if (result != UDS_SUCCESS) {
			return log_error_strerror(result,
						  "failed to update chapter %u index page %u",
						  chapter,
						  index_page_number);
		}
		expected_list_number = highest_delta_list + 1;
	}
	return UDS_SUCCESS;
}

/**
 * Add an entry to the volume index when rebuilding.
 *
 * @param index			  The index to query.
 * @param name			  The block name of interest.
 * @param virtual_chapter	  The virtual chapter number to write to the
 *				  volume index
 * @param will_be_sparse_chapter  True if this entry will be in the sparse
 *				  portion of the index at the end of
 *				  rebuilding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int replay_record(struct index *index,
			 const struct uds_chunk_name *name,
			 uint64_t virtual_chapter,
			 bool will_be_sparse_chapter)
{
	struct volume_index_record record;
	bool update_record;
	int result;
	if (will_be_sparse_chapter &&
	    !is_volume_index_sample(index->volume_index, name)) {
		// This entry will be in a sparse chapter after the rebuild
		// completes, and it is not a sample, so just skip over it.
		return UDS_SUCCESS;
	}

	result = get_volume_index_record(index->volume_index, name, &record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (record.is_found) {
		if (record.is_collision) {
			if (record.virtual_chapter == virtual_chapter) {
				/* The record is already correct, so we don't
				 * need to do anything */
				return UDS_SUCCESS;
			}
			update_record = true;
		} else if (record.virtual_chapter == virtual_chapter) {
			/*
			 * There is a volume index entry pointing to the
			 * current chapter, but we don't know if it is for the
			 * same name as the one we are currently working on or
			 * not. For now, we're just going to assume that it
			 * isn't. This will create one extra collision record
			 * if there was a deleted record in the current
			 * chapter.
			 */
			update_record = false;
		} else {
			/*
			 * If we're rebuilding, we don't normally want to go to
			 * disk to see if the record exists, since we will
			 * likely have just read the record from disk (i.e. we
			 * know it's there). The exception to this is when we
			 * already find an entry in the volume index that has a
			 * different chapter. In this case, we need to search
			 * that chapter to determine if the volume index entry
			 * was for the same record or a different one.
			 */
			result = search_volume_page_cache(index->volume,
							  NULL, name,
							  record.virtual_chapter,
							  NULL, &update_record);
			if (result != UDS_SUCCESS) {
				return result;
			}
		}
	} else {
		update_record = false;
	}

	if (update_record) {
		/*
		 * Update the volume index to reference the new chapter for the
		 * block. If the record had been deleted or dropped from the
		 * chapter index, it will be back.
		 */
		result = set_volume_index_record_chapter(&record,
							 virtual_chapter);
	} else {
		/*
		 * Add a new entry to the volume index referencing the open
		 * chapter. This should be done regardless of whether we are a
		 * brand new record or a sparse record, i.e. one that doesn't
		 * exist in the index but does on disk, since for a sparse
		 * record, we would want to un-sparsify if it did exist.
		 */
		result = put_volume_index_record(&record, virtual_chapter);
	}

	if ((result == UDS_DUPLICATE_NAME) || (result == UDS_OVERFLOW)) {
		/* Ignore duplicate record and delta list overflow errors */
		return UDS_SUCCESS;
	}

	return result;
}

/**********************************************************************/
void begin_save(struct index *index,
		bool checkpoint,
		uint64_t open_chapter_number)
{
	index->prev_checkpoint = index->last_checkpoint;
	index->last_checkpoint =
		((open_chapter_number == 0) ? NO_LAST_CHECKPOINT :
					      open_chapter_number - 1);

	log_info("beginning %s (vcn %llu)",
		 (checkpoint ? "checkpoint" : "save"),
		 index->last_checkpoint);
}

/**
 * Suspend the index if necessary and wait for a signal to resume.
 *
 * @param index	 The index to replay
 *
 * @return <code>true</code> if the replay should terminate
 **/
static bool check_for_suspend(struct index *index)
{
	bool ret_val;
	if (index->load_context == NULL) {
		return false;
	}

	lock_mutex(&index->load_context->mutex);
	if (index->load_context->status != INDEX_SUSPENDING) {
		unlock_mutex(&index->load_context->mutex);
		return false;
	}

	// Notify that we are suspended and wait for the resume.
	index->load_context->status = INDEX_SUSPENDED;
	broadcast_cond(&index->load_context->cond);

	while ((index->load_context->status != INDEX_OPENING) &&
	       (index->load_context->status != INDEX_FREEING)) {
		wait_cond(&index->load_context->cond,
			 &index->load_context->mutex);
	}

	ret_val = (index->load_context->status == INDEX_FREEING);
	unlock_mutex(&index->load_context->mutex);
	return ret_val;
}

/**********************************************************************/
int replay_volume(struct index *index, uint64_t from_vcn)
{
	int result;
	unsigned int j, k;
	enum index_lookup_mode old_lookup_mode;
	const struct geometry *geometry;
	uint64_t old_ipm_update, new_ipm_update, vcn;
	uint64_t upto_vcn = index->newest_virtual_chapter;
	log_info("Replaying volume from chapter %llu through chapter %llu",
		 from_vcn,
		 upto_vcn);
	set_volume_index_open_chapter(index->volume_index, upto_vcn);
	set_volume_index_open_chapter(index->volume_index, from_vcn);

	/*
	 * At least two cases to deal with here!
	 * - index loaded but replaying from last_checkpoint; maybe full, maybe
	 * not
	 * - index failed to load, full rebuild
	 *   Starts empty, then dense-only, then dense-plus-sparse.
	 *   Need to sparsify while processing individual chapters.
	 */
	old_lookup_mode = index->volume->lookup_mode;
	index->volume->lookup_mode = LOOKUP_FOR_REBUILD;
	/*
	 * Go through each record page of each chapter and add the records back
	 * to the volume index.	 This should not cause anything to be written
	 * to either the open chapter or on disk volume.  Also skip the on disk
	 * chapter corresponding to upto, as this would have already been
	 * purged from the volume index when the chapter was opened.
	 *
	 * Also, go through each index page for each chapter and rebuild the
	 * index page map.
	 */
	geometry = index->volume->geometry;
	old_ipm_update = get_last_update(index->volume->index_page_map);
	for (vcn = from_vcn; vcn < upto_vcn; ++vcn) {
		bool will_be_sparse_chapter;
		unsigned int chapter;
		if (check_for_suspend(index)) {
			log_info("Replay interrupted by index shutdown at chapter %llu",
				 vcn);
			return UDS_SHUTTINGDOWN;
		}

		will_be_sparse_chapter =
			is_chapter_sparse(geometry, from_vcn, upto_vcn, vcn);
		chapter = map_to_physical_chapter(geometry, vcn);
		prefetch_volume_pages(&index->volume->volume_store,
				      map_to_physical_page(geometry, chapter, 0),
				      geometry->pages_per_chapter);
		set_volume_index_open_chapter(index->volume_index, vcn);
		result = rebuild_index_page_map(index, vcn);
		if (result != UDS_SUCCESS) {
			index->volume->lookup_mode = old_lookup_mode;
			return log_error_strerror(result,
						  "could not rebuild index page map for chapter %u",
						  chapter);
		}

		for (j = 0; j < geometry->record_pages_per_chapter; j++) {
			byte *record_page;
			unsigned int record_page_number =
				geometry->index_pages_per_chapter + j;
			result = get_volume_page(index->volume, chapter,
						 record_page_number,
						 CACHE_PROBE_RECORD_FIRST,
						 &record_page, NULL);
			if (result != UDS_SUCCESS) {
				index->volume->lookup_mode = old_lookup_mode;
				return log_unrecoverable(result,
							 "could not get page %d",
							 record_page_number);
			}
			for (k = 0; k < geometry->records_per_page; k++) {
				const byte *name_bytes =
					record_page + (k * BYTES_PER_RECORD);

				struct uds_chunk_name name;
				memcpy(&name.name, name_bytes,
				       UDS_CHUNK_NAME_SIZE);

				result = replay_record(index, &name, vcn,
						       will_be_sparse_chapter);
				if (result != UDS_SUCCESS) {
					char hex_name[(2 * UDS_CHUNK_NAME_SIZE) +
						      1];
					if (chunk_name_to_hex(&name, hex_name,
							      sizeof(hex_name)) !=
					    UDS_SUCCESS) {
						strncpy(hex_name, "<unknown>",
							sizeof(hex_name));
					}
					index->volume->lookup_mode =
						old_lookup_mode;
					return log_unrecoverable(result,
								 "could not find block %s during rebuild",
								 hex_name);
				}
			}
		}
	}
	index->volume->lookup_mode = old_lookup_mode;

	// We also need to reap the chapter being replaced by the open chapter
	set_volume_index_open_chapter(index->volume_index, upto_vcn);

	new_ipm_update = get_last_update(index->volume->index_page_map);

	if (new_ipm_update != old_ipm_update) {
		log_info("replay changed index page map update from %llu to %llu",
			 old_ipm_update,
			 new_ipm_update);
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
void get_index_stats(struct index *index, struct uds_index_stats *counters)
{
	uint64_t cw_allocated =
		get_chapter_writer_memory_allocated(index->chapter_writer);
	// We're accessing the volume index while not on a zone thread, but
	// that's safe to do when acquiring statistics.
	struct volume_index_stats dense_stats, sparse_stats;
	get_volume_index_stats(index->volume_index, &dense_stats,
			       &sparse_stats);

	counters->entries_indexed =
		(dense_stats.record_count + sparse_stats.record_count);
	counters->memory_used =
		((uint64_t) dense_stats.memory_allocated +
		 (uint64_t) sparse_stats.memory_allocated +
		 (uint64_t) get_cache_size(index->volume) + cw_allocated);
	counters->collisions =
		(dense_stats.collision_count + sparse_stats.collision_count);
	counters->entries_discarded =
		(dense_stats.discard_count + sparse_stats.discard_count);
	counters->checkpoints = get_checkpoint_count(index->checkpoint);
}

/**********************************************************************/
void advance_active_chapters(struct index *index)
{
	index->newest_virtual_chapter++;
	if (are_same_physical_chapter(index->volume->geometry,
				      index->newest_virtual_chapter,
				      index->oldest_virtual_chapter)) {
		index->oldest_virtual_chapter++;
	}
}

/**********************************************************************/
uint64_t triage_index_request(struct index *index, Request *request)
{
	struct volume_index_triage triage;
	struct index_zone *zone;
	lookup_volume_index_name(index->volume_index, &request->chunk_name,
				 &triage);
	if (!triage.in_sampled_chapter) {
		// Not indexed or not a hook.
		return UINT64_MAX;
	}

	zone = get_request_zone(index, request);
	if (!is_zone_chapter_sparse(zone, triage.virtual_chapter)) {
		return UINT64_MAX;
	}

	// XXX Optimize for a common case by remembering the chapter from the
	// most recent barrier message and skipping this chapter if is it the
	// same.

	// Return the sparse chapter number to trigger the barrier messages.
	return triage.virtual_chapter;
}
