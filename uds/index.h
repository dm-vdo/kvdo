/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/krusty/src/uds/index.h#12 $
 */

#ifndef INDEX_H
#define INDEX_H

#include "chapterWriter.h"
#include "indexLayout.h"
#include "indexSession.h"
#include "indexZone.h"
#include "loadType.h"
#include "masterIndexOps.h"
#include "volume.h"


/**
 * Index checkpoint state private to indexCheckpoint.c.
 **/
struct index_checkpoint;

struct index {
	bool existed;
	bool has_saved_open_chapter;
	enum load_type loaded_type;
	IndexLoadContext *load_context;
	struct index_layout *layout;
	struct index_state *state;
	struct master_index *master_index;
	Volume *volume;
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
};

/**
 * Construct a new index from the given configuration.
 *
 * @param layout	The index layout
 * @param config	The configuration to use
 * @param user_params	The index session parameters.  If NULL, the default
 *			session parameters will be used.
 * @param zone_count	The number of zones for this index to use
 * @param load_type	How to create the index:  it can be create only, allow
 *			loading from files, and allow rebuilding from the
 *			volume
 * @param load_context	The load context to use
 * @param new_index	A pointer to hold a pointer to the new index
 *
 * @return	   UDS_SUCCESS or an error code
 **/
int __must_check make_index(struct index_layout *layout,
			    const struct configuration *config,
			    const struct uds_parameters *user_params,
			    unsigned int zone_count,
			    enum load_type load_type,
			    IndexLoadContext *load_context,
			    struct index **new_index);

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
int __must_check save_index(struct index *index);

/**
 * Clean up the index and its memory.
 *
 * @param index	  The index to destroy.
 **/
void free_index(struct index *index);

/**
 * Perform the index operation specified by the action field of a UDS request.
 *
 * For UDS API requests, this searches the index for the chunk name in the
 * request. If the chunk name is already present in the index, the location
 * field of the request will be set to the IndexRegion where it was found. If
 * the action is not DELETE, the oldMetadata field of the request will also be
 * filled in with the prior metadata for the name.
 *
 * If the API request action is:
 *
 *   REQUEST_INDEX, a record will be added to the open chapter with the
 *     metadata in the request for new records, and the existing metadata for
 *     existing records
 *
 *   REQUEST_UPDATE, a record will be added to the open chapter with the
 *     metadata in the request
 *
 *   REQUEST_QUERY, if the update flag is set in the request, any record
 *     found will be moved to the open chapter. In all other cases the contents
 *     of the index will remain unchanged.
 *
 *   REQUEST_REMOVE, the any entry with the name will removed from the index
 *
 * For non-API requests, no chunk name search is involved.
 *
 * @param index	      The index
 * @param request     The originating request
 *
 * @return UDS_SUCCESS, UDS_QUEUED, or an error code
 **/
int __must_check dispatch_index_request(struct index *index, Request *request);

/**
 * Internal helper to prepare the index for saving.
 *
 * @param index		       the index
 * @param checkpoint	       whether the save is a checkpoint
 * @param open_chapter_number  the virtual chapter number of the open chapter
 **/
void begin_save(struct index *index, bool checkpoint,
		uint64_t open_chapter_number);

/**
 * Replay the volume file to repopulate the master index.
 *
 * @param index		The index
 * @param from_vcn	The virtual chapter to start replaying
 *
 * @return		UDS_SUCCESS if successful
 **/
int __must_check replay_volume(struct index *index, uint64_t from_vcn);

/**
 * Gather statistics from the master index, volume, and cache.
 *
 * @param index	    The index
 * @param counters  the statistic counters for the index
 **/
void get_index_stats(struct index *index, struct uds_index_stats *counters);

/**
 * Set lookup state for this index.  Disabling lookups means assume
 * all records queried are new (intended for debugging uses, e.g.,
 * albfill).
 *
 * @param index	    The index
 * @param enabled   The new lookup state
 **/
void set_index_lookup_state(struct index *index, bool enabled);

/**
 * Advance the newest virtual chapter. If this will overwrite the oldest
 * virtual chapter, advance that also.
 *
 * @param index The index to advance
 **/
void advance_active_chapters(struct index *index);

/**
 * Triage an index request, deciding whether it requires that a sparse cache
 * barrier message precede it.
 *
 * This resolves the chunk name in the request in the master index,
 * determining if it is a hook or not, and if a hook, what virtual chapter (if
 * any) it might be found in. If a virtual chapter is found, it checks whether
 * that chapter appears in the sparse region of the index. If all these
 * conditions are met, the (sparse) virtual chapter number is returned. In all
 * other cases it returns <code>UINT64_MAX</code>.
 *
 * @param index	   the index that will process the request
 * @param request  the index request containing the chunk name to triage
 *
 * @return the sparse chapter number for the sparse cache barrier message, or
 *	   <code>UINT64_MAX</code> if the request does not require a barrier
 **/
uint64_t __must_check triage_index_request(struct index *index,
					   Request *request);

#endif /* INDEX_H */
