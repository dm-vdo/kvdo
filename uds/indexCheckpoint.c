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
 * $Id: //eng/uds-releases/lisa/src/uds/indexCheckpoint.c#1 $
 */

#include "indexCheckpoint.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds-threads.h"
#include "typeDefs.h"

/**
 * index checkpointState values
 *
 * @note The order of these values is significant,
 *       see indexState.c doIndexStateCheckpointInZone().
 **/
enum checkpoint_state {
	NOT_CHECKPOINTING,
	CHECKPOINT_IN_PROGRESS,
	CHECKPOINT_ABORTING
};

/**
 * Private structure which tracks checkpointing.
 **/
struct index_checkpoint {
	struct mutex mutex;           // covers this group of fields
	uint64_t chapter;             // vcn of the starting chapter
	enum checkpoint_state state;  // is checkpoint in progress or aborting
	unsigned int zones_busy;      // count of zones not yet done
	unsigned int frequency;       // number of chapters between checkpoints
	uint64_t checkpoints;         // number of checkpoints this session
};

/**
 * Enum return value of index checkpoint trigger function.
 **/
enum index_checkpoint_trigger_value {
	ICTV_IDLE,      //< no checkpointing right now
	ICTV_START,     //< start a new checkpoint now
	ICTV_CONTINUE,  //< continue checkpointing if needed
	ICTV_FINISH,    //< finish checkpointing, next time will start new cycle
	ICTV_ABORT      //< immediately abort checkpointing
};

typedef int checkpoint_function_t(struct uds_index *index, unsigned int zone);

//  These functions are called while holding the checkpoint->mutex but are
//  expected to release it.
//
static checkpoint_function_t do_checkpoint_start;
static checkpoint_function_t do_checkpoint_process;
static checkpoint_function_t do_checkpoint_finish;
static checkpoint_function_t do_checkpoint_abort;

checkpoint_function_t *const checkpoint_funcs[] = {
	NULL,
	do_checkpoint_start,
	do_checkpoint_process,
	do_checkpoint_finish,
	do_checkpoint_abort
};

/**********************************************************************/
int make_index_checkpoint(struct uds_index *index)
{
	struct index_checkpoint *checkpoint;
	int result = UDS_ALLOCATE(1,
				  struct index_checkpoint,
				  "struct index_checkpoint",
				  &checkpoint);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = uds_init_mutex(&checkpoint->mutex);
	if (result != UDS_SUCCESS) {
		UDS_FREE(checkpoint);
		return result;
	}

	checkpoint->checkpoints = 0;

	index->checkpoint = checkpoint;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_index_checkpoint(struct index_checkpoint *checkpoint)
{
	if (checkpoint != NULL) {
		uds_destroy_mutex(&checkpoint->mutex);
		UDS_FREE(checkpoint);
	}
}

/**********************************************************************/
unsigned int
get_index_checkpoint_frequency(struct index_checkpoint *checkpoint)
{
	unsigned int frequency;
	uds_lock_mutex(&checkpoint->mutex);
	frequency = checkpoint->frequency;
	uds_unlock_mutex(&checkpoint->mutex);
	return frequency;
}

/**********************************************************************/
unsigned int
set_index_checkpoint_frequency(struct index_checkpoint *checkpoint,
			       unsigned int frequency)
{
	unsigned int old_frequency;
	uds_lock_mutex(&checkpoint->mutex);
	old_frequency = checkpoint->frequency;
	checkpoint->frequency = frequency;
	uds_unlock_mutex(&checkpoint->mutex);
	return old_frequency;
}

/**********************************************************************/
uint64_t get_checkpoint_count(struct index_checkpoint *checkpoint)
{
	return checkpoint->checkpoints;
}

/**********************************************************************/
static enum index_checkpoint_trigger_value
get_checkpoint_action(struct index_checkpoint *checkpoint,
		      uint64_t virtual_chapter)
{
	unsigned int value;
	if (checkpoint->frequency == 0) {
		return ICTV_IDLE;
	}
	value = virtual_chapter % checkpoint->frequency;
	if (checkpoint->state == CHECKPOINT_ABORTING) {
		return ICTV_ABORT;
	} else if (checkpoint->state == CHECKPOINT_IN_PROGRESS) {
		if (value == checkpoint->frequency - 1) {
			return ICTV_FINISH;
		} else {
			return ICTV_CONTINUE;
		}
	} else {
		if (value == 0) {
			return ICTV_START;
		} else {
			return ICTV_IDLE;
		}
	}
}

/**********************************************************************/
int process_checkpointing(struct uds_index *index,
			  unsigned int zone,
			  uint64_t new_virtual_chapter)
{
	struct index_checkpoint *checkpoint = index->checkpoint;
	checkpoint_function_t *func;
	enum index_checkpoint_trigger_value ictv;
	uds_lock_mutex(&checkpoint->mutex);

	ictv = get_checkpoint_action(checkpoint, new_virtual_chapter);

	if (ictv == ICTV_START) {
		checkpoint->chapter = new_virtual_chapter;
	}

	func = checkpoint_funcs[ictv];
	if (func == NULL) {
		// nothing to do in idle state
		uds_unlock_mutex(&checkpoint->mutex);
		return UDS_SUCCESS;
	}

	return (*func)(index, zone);
}

/**********************************************************************/
int process_chapter_writer_checkpoint_saves(struct uds_index *index)
{
	struct index_checkpoint *checkpoint = index->checkpoint;

	int result = UDS_SUCCESS;

	uds_lock_mutex(&checkpoint->mutex);
	if (checkpoint->state == CHECKPOINT_IN_PROGRESS) {
		result = perform_index_state_checkpoint_chapter_synchronized_saves(index->state);

		if (result != UDS_SUCCESS) {
			checkpoint->state = CHECKPOINT_ABORTING;
			uds_log_info("checkpoint failed");
			index->last_checkpoint = index->prev_checkpoint;
		}
	}

	uds_unlock_mutex(&checkpoint->mutex);
	return result;
}

/**
 *  Helper function used to abort checkpoint if an error has occurred.
 *
 *  @param index        the index
 *  @param result       the error result
 *
 *  @return result
 **/
static int abort_checkpointing(struct uds_index *index, int result)
{
	if (index->checkpoint->state != NOT_CHECKPOINTING) {
		index->checkpoint->state = CHECKPOINT_ABORTING;
		uds_log_info("checkpoint failed");
		index->last_checkpoint = index->prev_checkpoint;
	}
	return result;
}

/**********************************************************************/
int finish_checkpointing(struct uds_index *index)
{
	unsigned int z;
	struct index_checkpoint *checkpoint = index->checkpoint;

	int result = process_chapter_writer_checkpoint_saves(index);
	if (result != UDS_SUCCESS) {
		return result;
	}

	uds_lock_mutex(&checkpoint->mutex);

	for (z = 0; z < index->zone_count; ++z) {
		if (checkpoint->state != CHECKPOINT_IN_PROGRESS) {
			break;
		}
		result = do_checkpoint_finish(index, z);
		// reacquire mutex released by do_checkpoint_finish
		uds_lock_mutex(&checkpoint->mutex);
		if (result != UDS_SUCCESS) {
			break;
		}
	}

	if ((result == UDS_SUCCESS) &&
	    (checkpoint->state == CHECKPOINT_IN_PROGRESS)) {
		result = finish_index_state_checkpoint(index->state);
		if (result == UDS_SUCCESS) {
			checkpoint->state = NOT_CHECKPOINTING;
		}
	}

	uds_unlock_mutex(&checkpoint->mutex);
	return result;
}

/**
 * Starts an incremental checkpoint.
 *
 * Called by the first zone to finish a chapter which starts a checkpoint.
 *
 * @param index the index
 * @param zone  the zone number
 *
 * @return UDS_SUCCESS or an error code
 **/
static int do_checkpoint_start(struct uds_index *index, unsigned int zone)
{
	int result;
	struct index_checkpoint *checkpoint = index->checkpoint;
	begin_save(index, true, checkpoint->chapter);
	result = start_index_state_checkpoint(index->state);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result,
				       "cannot start index checkpoint");
		index->last_checkpoint = index->prev_checkpoint;
		uds_unlock_mutex(&checkpoint->mutex);
		return result;
	}

	checkpoint->state = CHECKPOINT_IN_PROGRESS;
	checkpoint->zones_busy = index->zone_count;

	return do_checkpoint_process(index, zone);
}

/**********************************************************************/
static int do_checkpoint_process(struct uds_index *index, unsigned int zone)
{
	struct index_checkpoint *checkpoint = index->checkpoint;
	enum completion_status status = CS_NOT_COMPLETED;
	int result;
	uds_unlock_mutex(&checkpoint->mutex);
	result = perform_index_state_checkpoint_in_zone(index->state, zone,
							&status);
	if (result != UDS_SUCCESS) {
		uds_lock_mutex(&checkpoint->mutex);
		uds_log_error_strerror(result,
				       "cannot continue index checkpoint");
		result = abort_checkpointing(index, result);
		uds_unlock_mutex(&checkpoint->mutex);
	} else if (status == CS_JUST_COMPLETED) {
		uds_lock_mutex(&checkpoint->mutex);
		if (--checkpoint->zones_busy == 0) {
			checkpoint->checkpoints += 1;
			uds_log_info("finished checkpoint");
			result = finish_index_state_checkpoint(index->state);
			if (result != UDS_SUCCESS) {
				uds_log_error_strerror(result,
						       "%s checkpoint finish failed",
						       __func__);
			}
			checkpoint->state = NOT_CHECKPOINTING;
		}
		uds_unlock_mutex(&checkpoint->mutex);
	}
	return result;
}

/**********************************************************************/
static int do_checkpoint_abort(struct uds_index *index, unsigned int zone)
{
	struct index_checkpoint *checkpoint = index->checkpoint;
	enum completion_status status = CS_NOT_COMPLETED;
	int result = abort_index_state_checkpoint_in_zone(index->state, zone,
							  &status);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result,
				       "cannot abort index checkpoint");
	} else if (status == CS_JUST_COMPLETED) {
		if (--checkpoint->zones_busy == 0) {
			uds_log_info("aborted checkpoint");
			result = abort_index_state_checkpoint(index->state);
			if (result != UDS_SUCCESS) {
				uds_log_error_strerror(result,
						       "checkpoint abort failed");
			}
			checkpoint->state = NOT_CHECKPOINTING;
		}
	}
	uds_unlock_mutex(&checkpoint->mutex);

	return result;
}

/**********************************************************************/
static int do_checkpoint_finish(struct uds_index *index, unsigned int zone)
{
	struct index_checkpoint *checkpoint = index->checkpoint;
	enum completion_status status = CS_NOT_COMPLETED;
	int result;
	uds_unlock_mutex(&checkpoint->mutex);
	result = finish_index_state_checkpoint_in_zone(index->state, zone,
						       &status);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result,
				       "cannot finish index checkpoint");
		uds_lock_mutex(&checkpoint->mutex);
		result = abort_checkpointing(index, result);
		uds_unlock_mutex(&checkpoint->mutex);
	} else if (status == CS_JUST_COMPLETED) {
		uds_lock_mutex(&checkpoint->mutex);
		if (--checkpoint->zones_busy == 0) {
			checkpoint->checkpoints += 1;
			uds_log_info("finished checkpoint");
			result = finish_index_state_checkpoint(index->state);
			if (result != UDS_SUCCESS) {
				uds_log_error_strerror(result,
						       "%s checkpoint finish failed",
						       __func__);
			}
			checkpoint->state = NOT_CHECKPOINTING;
		}
		uds_unlock_mutex(&checkpoint->mutex);
	}
	return result;
}
