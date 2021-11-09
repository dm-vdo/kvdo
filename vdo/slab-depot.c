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
 */

#include "slab-depot.h"

#include <linux/atomic.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "action-manager.h"
#include "admin-state.h"
#include "block-allocator.h"
#include "completion.h"
#include "constants.h"
#include "header.h"
#include "num-utils.h"
#include "read-only-notifier.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-depot-format.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "slab-iterator.h"
#include "slab-summary.h"
#include "status-codes.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "vdo-state.h"

/**
 * Calculate the number of slabs a depot would have.
 *
 * @param depot  The depot
 *
 * @return The number of slabs
 **/
static
slab_count_t vdo_calculate_slab_count(struct slab_depot *depot)
{
	return compute_vdo_slab_count(depot->first_block, depot->last_block,
				      depot->slab_size_shift);
}

/**
 * Get an iterator over all the slabs in the depot.
 *
 * @param depot  The depot
 *
 * @return An iterator over the depot's slabs
 **/
static struct slab_iterator get_slab_iterator(struct slab_depot *depot)
{
	return vdo_iterate_slabs(depot->slabs, depot->slab_count - 1, 0, 1);
}

/**
 * Allocate a new slab pointer array. Any existing slab pointers will be
 * copied into the new array, and slabs will be allocated as needed. The
 * newly allocated slabs will not be distributed for use by the block
 * allocators.
 *
 * @param depot       The depot
 * @param slab_count  The number of slabs the depot should have in the new
 *                    array
 *
 * @return VDO_SUCCESS or an error code
 **/
static int allocate_slabs(struct slab_depot *depot, slab_count_t slab_count)
{
	block_count_t slab_size;
	bool resizing = false;
	physical_block_number_t slab_origin;
	block_count_t translation;

	int result = UDS_ALLOCATE(slab_count,
				  struct vdo_slab *,
				  "slab pointer array",
				  &depot->new_slabs);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (depot->slabs != NULL) {
		memcpy(depot->new_slabs,
		       depot->slabs,
		       depot->slab_count * sizeof(struct vdo_slab *));
		resizing = true;
	}

	slab_size = get_vdo_slab_config(depot)->slab_blocks;
	slab_origin = depot->first_block + (depot->slab_count * slab_size);

	/* The translation between allocator partition PBNs and layer PBNs. */
	translation = depot->origin - depot->first_block;
	depot->new_slab_count = depot->slab_count;
	while (depot->new_slab_count < slab_count) {
		struct block_allocator *allocator =
			depot->allocators[depot->new_slab_count %
					  depot->zone_count];
		struct vdo_slab **slab_ptr =
			&depot->new_slabs[depot->new_slab_count];
		result = make_vdo_slab(slab_origin,
				       allocator,
				       translation,
				       depot->vdo->recovery_journal,
				       depot->new_slab_count,
				       resizing,
				       slab_ptr);
		if (result != VDO_SUCCESS) {
			return result;
		}
		/*
		 * Increment here to ensure that vdo_abandon_new_slabs will 
		 * clean up correctly. 
		 */
		depot->new_slab_count++;

		slab_origin += slab_size;
	}

	return VDO_SUCCESS;
}

/**
 * Abandon any new slabs in this depot, freeing them as needed.
 *
 * @param depot  The depot
 **/
void vdo_abandon_new_slabs(struct slab_depot *depot)
{
	slab_count_t i;

	if (depot->new_slabs == NULL) {
		return;
	}

	for (i = depot->slab_count; i < depot->new_slab_count; i++) {
		free_vdo_slab(UDS_FORGET(depot->new_slabs[i]));
	}
	depot->new_slab_count = 0;
	depot->new_size = 0;
	UDS_FREE(UDS_FORGET(depot->new_slabs));
}

/**
 * Get the ID of the thread on which a given allocator operates.
 *
 * <p>Implements vdo_zone_thread_getter.
 **/
static thread_id_t get_allocator_thread_id(void *context,
					   zone_count_t zone_number)
{
	return vdo_get_block_allocator_for_zone(context, zone_number)->thread_id;
}

/**
 * Prepare to commit oldest tail blocks.
 *
 * <p>Implements vdo_action_preamble.
 **/
static void prepare_for_tail_block_commit(void *context,
					  struct vdo_completion *parent)
{
	struct slab_depot *depot = context;

	depot->active_release_request = depot->new_release_request;
	complete_vdo_completion(parent);
}

/**
 * Schedule a tail block commit if necessary. This method should not be called
 * directly. Rather, call schedule_vdo_default_action() on the depot's action
 * manager.
 *
 * <p>Implements vdo_action_scheduler.
 **/
static bool schedule_tail_block_commit(void *context)
{
	struct slab_depot *depot = context;

	if (depot->new_release_request == depot->active_release_request) {
		return false;
	}

	return schedule_vdo_action(depot->action_manager,
				   prepare_for_tail_block_commit,
				   release_vdo_tail_block_locks,
				   NULL,
				   NULL);
}

/**
 * Allocate those components of the slab depot which are needed only at load
 * time, not at format time.
 *
 * @param depot               The depot
 * @param summary_partition   The partition which holds the slab summary
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocate_components(struct slab_depot *depot,
			       struct partition *summary_partition)
{
	zone_count_t zone;
	slab_count_t slab_count, i;
	const struct thread_config *thread_config = depot->vdo->thread_config;
	int result = make_vdo_action_manager(depot->zone_count,
					     get_allocator_thread_id,
					     thread_config->journal_thread,
					     depot,
					     schedule_tail_block_commit,
					     depot->vdo,
					     &depot->action_manager);
	if (result != VDO_SUCCESS) {
		return result;
	}

	depot->origin = depot->first_block;

	result = make_vdo_slab_summary(depot->vdo,
				       summary_partition,
				       thread_config,
				       depot->slab_size_shift,
				       depot->slab_config.data_blocks,
				       depot->vdo->read_only_notifier,
				       &depot->slab_summary);
	if (result != VDO_SUCCESS) {
		return result;
	}

	slab_count = vdo_calculate_slab_count(depot);
	if (thread_config->physical_zone_count > slab_count) {
		return uds_log_error_strerror(VDO_BAD_CONFIGURATION,
					      "%u physical zones exceeds slab count %u",
					      thread_config->physical_zone_count,
					      slab_count);
	}

	/* Allocate the block allocators. */
	for (zone = 0; zone < depot->zone_count; zone++) {
		thread_id_t thread_id =
			vdo_get_physical_zone_thread(thread_config, zone);
		result = make_vdo_block_allocator(depot,
						  zone,
						  thread_id,
						  depot->vdo->states.vdo.nonce,
						  VIO_POOL_SIZE,
						  depot->vdo,
						  depot->vdo->read_only_notifier,
						  &depot->allocators[zone]);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	/* Allocate slabs. */
	result = allocate_slabs(depot, slab_count);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/* Use the new slabs. */
	for (i = depot->slab_count; i < depot->new_slab_count; i++) {
		struct vdo_slab *slab = depot->new_slabs[i];

		register_vdo_slab_with_allocator(slab->allocator, slab);
		WRITE_ONCE(depot->slab_count, depot->slab_count + 1);
	}

	depot->slabs = depot->new_slabs;
	depot->new_slabs = NULL;
	depot->new_slab_count = 0;

	return VDO_SUCCESS;
}

/**
 * Make a slab depot and configure it with the state read from the super block.
 *
 * @param [in]  state               The slab depot state from the super block
 * @param [in]  vdo                 The VDO which will own the depot
 * @param [in]  summary_partition   The partition which holds the slab summary
 * @param [out] depot_ptr           A pointer to hold the depot
 *
 * @return A success or error code
 **/
int decode_vdo_slab_depot(struct slab_depot_state_2_0 state,
			  struct vdo *vdo,
			  struct partition *summary_partition,
			  struct slab_depot **depot_ptr)
{
	unsigned int slab_size_shift;
	struct slab_depot *depot;
	int result;

	/*
	 * Calculate the bit shift for efficiently mapping block numbers to 
	 * slabs. Using a shift requires that the slab size be a power of two. 
	 */
	block_count_t slab_size = state.slab_config.slab_blocks;

	if (!is_power_of_2(slab_size)) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "slab size must be a power of two");
	}
	slab_size_shift = log_base_two(slab_size);

	result = UDS_ALLOCATE_EXTENDED(struct slab_depot,
				       vdo->thread_config->physical_zone_count,
				       struct block_allocator *,
				       __func__,
				       &depot);
	if (result != VDO_SUCCESS) {
		return result;
	}

	depot->vdo = vdo;
	depot->old_zone_count = state.zone_count;
	depot->zone_count = vdo->thread_config->physical_zone_count;
	depot->slab_config = state.slab_config;
	depot->first_block = state.first_block;
	depot->last_block = state.last_block;
	depot->slab_size_shift = slab_size_shift;

	result = allocate_components(depot, summary_partition);
	if (result != VDO_SUCCESS) {
		free_vdo_slab_depot(depot);
		return result;
	}

	*depot_ptr = depot;
	return VDO_SUCCESS;
}

/**
 * Destroy a slab depot.
 *
 * @param depot  The depot to destroy
 **/
void free_vdo_slab_depot(struct slab_depot *depot)
{
	zone_count_t zone = 0;

	if (depot == NULL) {
		return;
	}

	vdo_abandon_new_slabs(depot);

	for (zone = 0; zone < depot->zone_count; zone++) {
		free_vdo_block_allocator(UDS_FORGET(depot->allocators[zone]));
	}

	if (depot->slabs != NULL) {
		slab_count_t i;

		for (i = 0; i < depot->slab_count; i++) {
			free_vdo_slab(UDS_FORGET(depot->slabs[i]));
		}
	}

	UDS_FREE(UDS_FORGET(depot->slabs));
	UDS_FREE(UDS_FORGET(depot->action_manager));
	free_vdo_slab_summary(UDS_FORGET(depot->slab_summary));
	UDS_FREE(depot);
}

/**
 * Record the state of a slab depot for encoding into the super block.
 *
 * @param depot   The depot to encode
 *
 * @return The depot state
 **/
struct slab_depot_state_2_0 record_vdo_slab_depot(const struct slab_depot *depot)
{
	/*
	 * If this depot is currently using 0 zones, it must have been
	 * synchronously loaded by a tool and is now being saved. We
	 * did not load and combine the slab summary, so we still need
	 * to do that next time we load with the old zone count rather
	 * than 0.
	 */
	struct slab_depot_state_2_0 state;
	zone_count_t zones_to_record = depot->zone_count;

	if (depot->zone_count == 0) {
		zones_to_record = depot->old_zone_count;
	}

	state = (struct slab_depot_state_2_0) {
		.slab_config = depot->slab_config,
		.first_block = depot->first_block,
		.last_block = depot->last_block,
		.zone_count = zones_to_record,
	};

	return state;
}

/**
 * Allocate the ref_counts for all slabs in the depot. This method may be
 * called only before entering normal operation from the load thread.
 *
 * @param depot  The depot whose ref_counts need allocation
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_allocate_slab_ref_counts(struct slab_depot *depot)
{
	struct slab_iterator iterator = get_slab_iterator(depot);

	while (vdo_has_next_slab(&iterator)) {
		int result =
			allocate_ref_counts_for_vdo_slab(vdo_next_slab(&iterator));
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	return VDO_SUCCESS;
}

/**
 * Get the block allocator for a specified physical zone from a depot.
 *
 * @param depot        The depot
 * @param zone_number  The physical zone
 *
 * @return The block allocator for the specified zone
 **/
struct block_allocator *vdo_get_block_allocator_for_zone(struct slab_depot *depot,
							 zone_count_t zone_number)
{
	return depot->allocators[zone_number];
}

/**
 * Get the number of the slab that contains a specified block.
 *
 * @param depot            The slab depot
 * @param pbn              The physical block number
 * @param slab_number_ptr  A pointer to hold the slab number
 *
 * @return VDO_SUCCESS or an error
 **/
static
int vdo_get_slab_number(const struct slab_depot *depot,
			physical_block_number_t pbn,
			slab_count_t *slab_number_ptr)
{
	slab_count_t slab_number;

	if (pbn < depot->first_block) {
		return VDO_OUT_OF_RANGE;
	}

	slab_number = (pbn - depot->first_block) >> depot->slab_size_shift;
	if (slab_number >= depot->slab_count) {
		return VDO_OUT_OF_RANGE;
	}

	*slab_number_ptr = slab_number;
	return VDO_SUCCESS;
}

/**
 * Get the slab object for the slab that contains a specified block. Will put
 * the VDO in read-only mode if the PBN is not a valid data block nor the zero
 * block.
 *
 * @param depot  The slab depot
 * @param pbn    The physical block number
 *
 * @return The slab containing the block, or NULL if the block number is the
 *         zero block or otherwise out of range
 **/
struct vdo_slab *get_vdo_slab(const struct slab_depot *depot,
			      physical_block_number_t pbn)
{
	slab_count_t slab_number;
	int result;

	if (pbn == VDO_ZERO_BLOCK) {
		return NULL;
	}

	result = vdo_get_slab_number(depot, pbn, &slab_number);
	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(depot->vdo->read_only_notifier, result);
		return NULL;
	}

	return depot->slabs[slab_number];
}

/**
 * Get the slab journal for the slab that contains a specified block.
 *
 * @param depot  The slab depot
 * @param pbn    The physical block number within the block depot partition
 *               of any block in the slab
 *
 * @return The slab journal of the slab containing the block, or NULL if the
 *         block number is for the zero block or otherwise out of range
 **/
struct slab_journal *get_vdo_slab_journal(const struct slab_depot *depot,
					  physical_block_number_t pbn)
{
	struct vdo_slab *slab = get_vdo_slab(depot, pbn);

	return ((slab != NULL) ? slab->journal : NULL);
}

/**
 * Determine how many new references a block can acquire. This method must be
 * called from the physical zone thread of the PBN.
 *
 * @param depot  The slab depot
 * @param pbn    The physical block number that is being queried
 *
 * @return the number of available references
 **/
uint8_t vdo_get_increment_limit(struct slab_depot *depot,
				physical_block_number_t pbn)
{
	struct vdo_slab *slab = get_vdo_slab(depot, pbn);

	if ((slab == NULL) || vdo_is_unrecovered_slab(slab)) {
		return 0;
	}

	return vdo_get_available_references(slab->reference_counts, pbn);
}

/**
 * Determine whether the given PBN refers to a data block.
 *
 * @param depot  The depot
 * @param pbn    The physical block number to ask about
 *
 * @return <code>True</code> if the PBN corresponds to a data block
 **/
bool vdo_is_physical_data_block(const struct slab_depot *depot,
				physical_block_number_t pbn)
{
	slab_count_t slab_number;
	slab_block_number sbn;
	int result;

	if (pbn == VDO_ZERO_BLOCK) {
		return true;
	}

	if (vdo_get_slab_number(depot, pbn, &slab_number) != VDO_SUCCESS) {
		return false;
	}

	result = vdo_slab_block_number_from_pbn(depot->slabs[slab_number],
						pbn, &sbn);
	return (result == VDO_SUCCESS);
}

/**
 * Get the total number of data blocks allocated across all the slabs in the
 * depot, which is the total number of blocks with a non-zero reference count.
 * This may be called from any thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of blocks with a non-zero reference count
 **/
block_count_t get_vdo_slab_depot_allocated_blocks(const struct slab_depot *depot)
{
	block_count_t total = 0;
	zone_count_t zone;

	for (zone = 0; zone < depot->zone_count; zone++) {
		/* The allocators are responsible for thread safety. */
		total += get_vdo_allocated_blocks(depot->allocators[zone]);
	}
	return total;
}

/**
 * Get the total number of data blocks in all the slabs in the depot. This may
 * be called from any thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of data blocks in all slabs
 **/
block_count_t get_vdo_slab_depot_data_blocks(const struct slab_depot *depot)
{
	return (READ_ONCE(depot->slab_count) * depot->slab_config.data_blocks);
}

/**
 * Get the total number of unrecovered slabs in the depot, which is the total
 * number of unrecovered slabs from all zones. This may be called from any
 * thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of slabs that are unrecovered
 **/
static
slab_count_t get_vdo_slab_depot_unrecovered_slab_count(const struct slab_depot *depot)
{
	slab_count_t total = 0;
	zone_count_t zone;

	for (zone = 0; zone < depot->zone_count; zone++) {
		/* The allocators are responsible for thread safety. */
		total += get_vdo_unrecovered_slab_count(depot->allocators[zone]);
	}
	return total;
}

/**
 * The preamble of a load operation which loads the slab summary.
 *
 * <p>Implements vdo_action_preamble.
 **/
static void start_depot_load(void *context, struct vdo_completion *parent)
{
	struct slab_depot *depot = context;

	load_vdo_slab_summary(depot->slab_summary,
			      get_current_vdo_manager_operation(depot->action_manager),
			      depot->old_zone_count,
			      parent);
}

/**
 * Asynchronously load any slab depot state that isn't included in the
 * super_block component. This method may be called only before entering
 * normal operation from the load thread.
 *
 * @param depot        The depot to load
 * @param operation    The type of load to perform
 * @param parent       The completion to finish when the load is complete
 * @param context      Additional context for the load operation; may be NULL
 **/
void load_vdo_slab_depot(struct slab_depot *depot,
			 const struct admin_state_code *operation,
			 struct vdo_completion *parent,
			 void *context)
{
	if (assert_vdo_load_operation(operation, parent)) {
		schedule_vdo_operation_with_context(depot->action_manager,
						    operation,
						    start_depot_load,
						    load_vdo_block_allocator,
						    NULL,
						    context,
						    parent);
	}
}

/**
 * Prepare the slab depot to come online and start allocating blocks. This
 * method may be called only before entering normal operation from the load
 * thread. It must be called before allocation may proceed.
 *
 * @param depot       The depot to prepare
 * @param load_type  The load type
 * @param parent      The completion to finish when the operation is complete
 **/
void prepare_vdo_slab_depot_to_allocate(struct slab_depot *depot,
					enum slab_depot_load_type load_type,
					struct vdo_completion *parent)
{
	depot->load_type = load_type;
	atomic_set(&depot->zones_to_scrub, depot->zone_count);
	schedule_vdo_action(depot->action_manager,
			    NULL,
			    prepare_vdo_block_allocator_to_allocate,
			    NULL,
			    parent);
}

/**
 * Update the slab depot to reflect its new size in memory. This size is saved
 * to disk as part of the super block.
 *
 * @param depot  The depot to update
 **/
void update_vdo_slab_depot_size(struct slab_depot *depot)
{
	depot->last_block = depot->new_last_block;
}

/**
 * Allocate new memory needed for a resize of a slab depot to the given size.
 *
 * @param depot     The depot to prepare to resize
 * @param new_size  The number of blocks in the new depot
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_prepare_to_grow_slab_depot(struct slab_depot *depot, block_count_t new_size)
{
	struct slab_depot_state_2_0 new_state;
	int result;
	slab_count_t new_slab_count;

	if ((new_size >> depot->slab_size_shift) <= depot->slab_count) {
		return VDO_INCREMENT_TOO_SMALL;
	}

	/* Generate the depot configuration for the new block count. */
	result = configure_vdo_slab_depot(new_size,
					  depot->first_block,
					  depot->slab_config,
					  depot->zone_count,
					  &new_state);
	if (result != VDO_SUCCESS) {
		return result;
	}

	new_slab_count = compute_vdo_slab_count(depot->first_block,
						new_state.last_block,
						depot->slab_size_shift);
	if (new_slab_count <= depot->slab_count) {
		return uds_log_error_strerror(VDO_INCREMENT_TOO_SMALL,
					      "Depot can only grow");
	}
	if (new_slab_count == depot->new_slab_count) {
		/* Check it out, we've already got all the new slabs allocated! */
		return VDO_SUCCESS;
	}

	vdo_abandon_new_slabs(depot);
	result = allocate_slabs(depot, new_slab_count);
	if (result != VDO_SUCCESS) {
		vdo_abandon_new_slabs(depot);
		return result;
	}

	depot->new_size = new_size;
	depot->old_last_block = depot->last_block;
	depot->new_last_block = new_state.last_block;

	return VDO_SUCCESS;
}

/**
 * Finish registering new slabs now that all of the allocators have received
 * their new slabs.
 *
 * <p>Implements vdo_action_conclusion.
 **/
static int finish_registration(void *context)
{
	struct slab_depot *depot = context;

	WRITE_ONCE(depot->slab_count, depot->new_slab_count);
	UDS_FREE(depot->slabs);
	depot->slabs = depot->new_slabs;
	depot->new_slabs = NULL;
	depot->new_slab_count = 0;
	return VDO_SUCCESS;
}

/**
 * Use the new slabs allocated for resize.
 *
 * @param depot   The depot
 * @param parent  The object to notify when complete
 **/
void vdo_use_new_slabs(struct slab_depot *depot, struct vdo_completion *parent)
{
	ASSERT_LOG_ONLY(depot->new_slabs != NULL, "Must have new slabs to use");
	schedule_vdo_operation(depot->action_manager,
			       VDO_ADMIN_STATE_SUSPENDED_OPERATION,
			       NULL,
			       register_new_vdo_slabs_for_allocator,
			       finish_registration,
			       parent);
}

/**
 * Drain all slab depot I/O. If saving, or flushing, all dirty depot metadata
 * will be written out. If saving or suspending, the depot will be left in a
 * suspended state.
 *
 * @param depot      The depot to drain
 * @param operation  The drain operation (flush, rebuild, suspend, or save)
 * @param parent     The completion to finish when the drain is complete
 **/
void drain_vdo_slab_depot(struct slab_depot *depot,
			  const struct admin_state_code *operation,
			  struct vdo_completion *parent)
{
	schedule_vdo_operation(depot->action_manager,
			       operation,
			       NULL,
			       drain_vdo_block_allocator,
			       NULL,
			       parent);
}

/**
 * Resume a suspended slab depot.
 *
 * @param depot   The depot to resume
 * @param parent  The completion to finish when the depot has resumed
 **/
void resume_vdo_slab_depot(struct slab_depot *depot, struct vdo_completion *parent)
{
	if (vdo_is_read_only(depot->vdo->read_only_notifier)) {
		vdo_finish_completion(parent, VDO_READ_ONLY);
		return;
	}

	schedule_vdo_operation(depot->action_manager,
			       VDO_ADMIN_STATE_RESUMING,
			       NULL,
			       resume_vdo_block_allocator,
			       NULL,
			       parent);
}

/**
 * Commit all dirty tail blocks which are locking a given recovery journal
 * block. This method must be called from the journal zone thread.
 *
 * @param depot                  The depot
 * @param recovery_block_number  The sequence number of the recovery journal
 *                               block whose locks should be released
 **/
void
vdo_commit_oldest_slab_journal_tail_blocks(struct slab_depot *depot,
					   sequence_number_t recovery_block_number)
{
	if (depot == NULL) {
		return;
	}

	depot->new_release_request = recovery_block_number;
	schedule_vdo_default_action(depot->action_manager);
}

/**
 * Get the slab_config of a depot.
 *
 * @param depot  The slab depot
 *
 * @return The slab configuration of the specified depot
 **/
const struct slab_config *get_vdo_slab_config(const struct slab_depot *depot)
{
	return &depot->slab_config;
}

/**
 * Get the slab summary.
 *
 * @param depot  The slab depot
 *
 * @return The slab summary
 **/
struct slab_summary *get_vdo_slab_summary(const struct slab_depot *depot)
{
	return depot->slab_summary;
}

/**
 * Get the portion of the slab summary for a given physical zone.
 *
 * @param depot  The slab depot
 * @param zone   The zone
 *
 * @return The portion of the slab summary for the specified zone
 **/
struct slab_summary_zone *
get_vdo_slab_summary_for_zone(const struct slab_depot *depot, zone_count_t zone)
{
	if (depot->slab_summary == NULL) {
		return NULL;
	}

	return vdo_get_slab_summary_for_zone(depot->slab_summary, zone);
}

/**
 * Scrub all unrecovered slabs.
 *
 * @param depot         The depot to scrub
 * @param parent        The object to notify when scrubbing has been launched
 *                      for all zones
 **/
void vdo_scrub_all_unrecovered_slabs(struct slab_depot *depot,
				     struct vdo_completion *parent)
{
	schedule_vdo_action(depot->action_manager,
			    NULL,
			    scrub_all_unrecovered_vdo_slabs_in_zone,
			    NULL,
			    parent);
}

/**
 * Notify a slab depot that one of its allocators has finished scrubbing slabs.
 * This method should only be called if the scrubbing was successful. This
 * callback is registered by each block allocator in
 * scrub_all_unrecovered_vdo_slabs_in_zone().
 *
 * @param completion  A completion whose parent must be a slab depot
 **/
void vdo_notify_zone_finished_scrubbing(struct vdo_completion *completion)
{
	enum vdo_state prior_state;

	struct slab_depot *depot = completion->parent;

	if (atomic_add_return(-1, &depot->zones_to_scrub) > 0) {
		return;
	}

	/* We're the last! */
	prior_state = atomic_cmpxchg(&depot->vdo->state,
				     VDO_RECOVERING, VDO_DIRTY);
	/*
	 * To be safe, even if the CAS failed, ensure anything that follows is 
	 * ordered with respect to whatever state change did happen. 
	 */
	smp_mb__after_atomic();

	/*
	 * We must check the VDO state here and not the depot's
	 * read_only_notifier since the compare-swap-above could have
	 * failed due to a read-only entry which our own thread does not
	 * yet know about.
	 */
	if (prior_state == VDO_DIRTY) {
		uds_log_info("VDO commencing normal operation");
	} else if (prior_state == VDO_RECOVERING) {
		uds_log_info("Exiting recovery mode");
	}
}

/**
 * Get the physical size to which this depot is prepared to grow.
 *
 * @param depot  The slab depot
 *
 * @return The new number of blocks the depot will be grown to, or 0 if the
 *         depot is not prepared to grow
 **/
block_count_t get_vdo_slab_depot_new_size(const struct slab_depot *depot)
{
	return (depot->new_slabs == NULL) ? 0 : depot->new_size;
}


/**
 * Get the total of the statistics from all the block allocators in the depot.
 *
 * @param depot  The slab depot
 *
 * @return The statistics from all block allocators in the depot
 **/
static struct block_allocator_statistics __must_check
get_depot_block_allocator_statistics(const struct slab_depot *depot)
{
	struct block_allocator_statistics totals;
	zone_count_t zone;

	memset(&totals, 0, sizeof(totals));

	for (zone = 0; zone < depot->zone_count; zone++) {
		struct block_allocator *allocator = depot->allocators[zone];
		struct block_allocator_statistics stats =
			get_vdo_block_allocator_statistics(allocator);
		totals.slab_count += stats.slab_count;
		totals.slabs_opened += stats.slabs_opened;
		totals.slabs_reopened += stats.slabs_reopened;
	}

	return totals;
}

/**
 * Get the cumulative ref_counts statistics for the depot.
 *
 * @param depot  The slab depot
 *
 * @return The cumulative statistics for all ref_counts in the depot
 **/
static struct ref_counts_statistics __must_check
get_depot_ref_counts_statistics(const struct slab_depot *depot)
{
	struct ref_counts_statistics depot_stats;
	zone_count_t zone;

	memset(&depot_stats, 0, sizeof(depot_stats));

	for (zone = 0; zone < depot->zone_count; zone++) {
		struct block_allocator *allocator = depot->allocators[zone];
		struct ref_counts_statistics stats =
			get_vdo_ref_counts_statistics(allocator);
		depot_stats.blocks_written += stats.blocks_written;
	}

	return depot_stats;
}

/**
 * Get the aggregated slab journal statistics for the depot.
 *
 * @param depot  The slab depot
 *
 * @return The aggregated statistics for all slab journals in the depot
 **/
static struct slab_journal_statistics __must_check
get_depot_slab_journal_statistics(const struct slab_depot *depot)
{
	struct slab_journal_statistics depot_stats;
	zone_count_t zone;

	memset(&depot_stats, 0, sizeof(depot_stats));

	for (zone = 0; zone < depot->zone_count; zone++) {
		struct block_allocator *allocator = depot->allocators[zone];
		struct slab_journal_statistics stats =
			get_vdo_slab_journal_statistics(allocator);
		depot_stats.disk_full_count += stats.disk_full_count;
		depot_stats.flush_count += stats.flush_count;
		depot_stats.blocked_count += stats.blocked_count;
		depot_stats.blocks_written += stats.blocks_written;
		depot_stats.tail_busy_count += stats.tail_busy_count;
	}

	return depot_stats;
}

/**
 * Get all the vdo_statistics fields that are properties of the slab depot.
 *
 * @param depot  The slab depot
 * @param stats  The vdo statistics structure to partially fill
 **/
void get_vdo_slab_depot_statistics(const struct slab_depot *depot,
				   struct vdo_statistics *stats)
{
	slab_count_t slab_count = READ_ONCE(depot->slab_count);
	slab_count_t unrecovered =
		get_vdo_slab_depot_unrecovered_slab_count(depot);

	stats->recovery_percentage =
		(slab_count - unrecovered) * 100 / slab_count;
	stats->allocator = get_depot_block_allocator_statistics(depot);
	stats->ref_counts = get_depot_ref_counts_statistics(depot);
	stats->slab_journal = get_depot_slab_journal_statistics(depot);
	stats->slab_summary =
		get_vdo_slab_summary_statistics(depot->slab_summary);
}

/**
 * Dump the slab depot, in a thread-unsafe fashion.
 *
 * @param depot  The slab depot
 **/
void dump_vdo_slab_depot(const struct slab_depot *depot)
{
	uds_log_info("vdo slab depot");
	uds_log_info("  zone_count=%u old_zone_count=%u slabCount=%u active_release_request=%llu new_release_request=%llu",
		     (unsigned int) depot->zone_count,
		     (unsigned int) depot->old_zone_count,
		     READ_ONCE(depot->slab_count),
		     (unsigned long long) depot->active_release_request,
		     (unsigned long long) depot->new_release_request);
}
