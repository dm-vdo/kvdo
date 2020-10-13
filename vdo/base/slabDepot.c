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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepot.c#79 $
 */

#include "slabDepot.h"

#include "atomicDefs.h"
#include "logger.h"
#include "memoryAlloc.h"

#include "actionManager.h"
#include "adminState.h"
#include "blockAllocatorInternals.h"
#include "constants.h"
#include "header.h"
#include "numUtils.h"
#include "readOnlyNotifier.h"
#include "refCounts.h"
#include "slab.h"
#include "slabDepotFormat.h"
#include "slabDepotInternals.h"
#include "slabJournal.h"
#include "slabIterator.h"
#include "slabSummary.h"
#include "threadConfig.h"
#include "types.h"
#include "vdoState.h"

/**********************************************************************/
slab_count_t calculate_slab_count(struct slab_depot *depot)
{
	return compute_slab_count(depot->first_block, depot->last_block,
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
	return iterate_slabs(depot->slabs, depot->slab_count - 1, 0, 1);
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
	int result = ALLOCATE(slab_count,
			      struct vdo_slab *,
			      "slab pointer array",
			      &depot->new_slabs);
	if (result != VDO_SUCCESS) {
		return result;
	}

	bool resizing = false;
	if (depot->slabs != NULL) {
		memcpy(depot->new_slabs,
		       depot->slabs,
		       depot->slab_count * sizeof(struct vdo_slab *));
		resizing = true;
	}

	block_count_t slab_size = get_slab_config(depot)->slab_blocks;
	physical_block_number_t slab_origin =
		depot->first_block + (depot->slab_count * slab_size);

	// The translation between allocator partition PBNs and layer PBNs.
	block_count_t translation = depot->origin - depot->first_block;
	depot->new_slab_count = depot->slab_count;
	while (depot->new_slab_count < slab_count) {
		struct block_allocator *allocator =
			depot->allocators[depot->new_slab_count %
					  depot->zone_count];
		struct vdo_slab **slab_ptr =
			&depot->new_slabs[depot->new_slab_count];
		result = make_slab(slab_origin,
				   allocator,
				   translation,
				   depot->journal,
				   depot->new_slab_count,
				   resizing,
				   slab_ptr);
		if (result != VDO_SUCCESS) {
			return result;
		}
		// Increment here to ensure that abandon_new_slabs will clean up
		// correctly.
		depot->new_slab_count++;

		slab_origin += slab_size;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
void abandon_new_slabs(struct slab_depot *depot)
{
	if (depot->new_slabs == NULL) {
		return;
	}
	slab_count_t i;
	for (i = depot->slab_count; i < depot->new_slab_count; i++) {
		free_slab(&depot->new_slabs[i]);
	}
	depot->new_slab_count = 0;
	FREE(depot->new_slabs);
	depot->new_slabs = NULL;
	depot->new_size = 0;
}

/**
 * Get the ID of the thread on which a given allocator operates.
 *
 * <p>Implements ZoneThreadGetter.
 **/
static thread_id_t get_allocator_thread_id(void *context,
					   zone_count_t zone_number)
{
	return get_block_allocator_for_zone(context, zone_number)->thread_id;
}

/**
 * Prepare to commit oldest tail blocks.
 *
 * <p>Implements ActionPreamble.
 **/
static void prepare_for_tail_block_commit(void *context,
					  struct vdo_completion *parent)
{
	struct slab_depot *depot = context;
	depot->active_release_request = depot->new_release_request;
	complete_completion(parent);
}

/**
 * Schedule a tail block commit if necessary. This method should not be called
 * directly. Rather, call schedule_default_action() on the depot's action
 * manager.
 *
 * <p>Implements ActionScheduler,
 **/
static bool schedule_tail_block_commit(void *context)
{
	struct slab_depot *depot = context;
	if (depot->new_release_request == depot->active_release_request) {
		return false;
	}

	return schedule_action(depot->action_manager,
			       prepare_for_tail_block_commit,
			       release_tail_block_locks,
			       NULL,
			       NULL);
}

/**
 * Allocate those components of the slab depot which are needed only at load
 * time, not at format time.
 *
 * @param depot               The depot
 * @param nonce               The nonce of the VDO
 * @param thread_config       The thread config of the VDO
 * @param vio_pool_size       The size of the VIO pool
 * @param layer               The physical layer below this depot
 * @param summary_partition   The partition which holds the slab summary
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocate_components(struct slab_depot *depot,
			       nonce_t nonce,
			       const struct thread_config *thread_config,
			       block_count_t vio_pool_size,
			       PhysicalLayer *layer,
			       struct partition *summary_partition)
{
	int result = make_action_manager(depot->zone_count,
					 get_allocator_thread_id,
					 get_journal_zone_thread(thread_config),
					 depot,
					 schedule_tail_block_commit,
					 layer,
					 &depot->action_manager);
	if (result != VDO_SUCCESS) {
		return result;
	}

	depot->origin = depot->first_block;

	result = make_slab_summary(layer,
				   summary_partition,
				   thread_config,
				   depot->slab_size_shift,
				   depot->slab_config.data_blocks,
				   depot->read_only_notifier,
				   &depot->slab_summary);
	if (result != VDO_SUCCESS) {
		return result;
	}

	slab_count_t slab_count = calculate_slab_count(depot);
	if (thread_config->physical_zone_count > slab_count) {
		return log_error_strerror(VDO_BAD_CONFIGURATION,
					  "%u physical zones exceeds slab count %u",
					  thread_config->physical_zone_count,
					  slab_count);
	}

	// Allocate the block allocators.
	zone_count_t zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		thread_id_t thread_id =
			get_physical_zone_thread(thread_config, zone);
		result = make_block_allocator(depot,
					      zone,
					      thread_id,
					      nonce,
					      vio_pool_size,
					      layer,
					      depot->read_only_notifier,
					      &depot->allocators[zone]);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	// Allocate slabs.
	result = allocate_slabs(depot, slab_count);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Use the new slabs.
	slab_count_t i;
	for (i = depot->slab_count; i < depot->new_slab_count; i++) {
		struct vdo_slab *slab = depot->new_slabs[i];
		register_slab_with_allocator(slab->allocator, slab);
		depot->slab_count++;
	}

	depot->slabs = depot->new_slabs;
	depot->new_slabs = NULL;
	depot->new_slab_count = 0;

	return VDO_SUCCESS;
}

/**********************************************************************/
int decode_slab_depot(struct slab_depot_state_2_0 state,
		      const struct thread_config *thread_config,
		      nonce_t nonce,
		      PhysicalLayer *layer,
		      struct partition *summary_partition,
		      struct read_only_notifier *read_only_notifier,
		      struct recovery_journal *recovery_journal,
		      Atomic32 *vdo_state,
		      struct slab_depot **depot_ptr)
{
	// Calculate the bit shift for efficiently mapping block numbers to
	// slabs. Using a shift requires that the slab size be a power of two.
	block_count_t slab_size = state.slab_config.slab_blocks;
	if (!is_power_of_2(slab_size)) {
		return log_error_strerror(UDS_INVALID_ARGUMENT,
					  "slab size must be a power of two");
	}
	unsigned int slab_size_shift = log_base_two(slab_size);

	struct slab_depot *depot;
	int result = ALLOCATE_EXTENDED(struct slab_depot,
				       thread_config->physical_zone_count,
				       struct block_allocator *,
				       __func__,
				       &depot);
	if (result != VDO_SUCCESS) {
		return result;
	}

	depot->old_zone_count = state.zone_count;
	depot->zone_count = thread_config->physical_zone_count;
	depot->slab_config = state.slab_config;
	depot->read_only_notifier = read_only_notifier;
	depot->first_block = state.first_block;
	depot->last_block = state.last_block;
	depot->slab_size_shift = slab_size_shift;
	depot->journal = recovery_journal;
	depot->vdo_state = vdo_state;

	result = allocate_components(depot,
				     nonce,
				     thread_config,
				     VIO_POOL_SIZE,
				     layer,
				     summary_partition);
	if (result != VDO_SUCCESS) {
		free_slab_depot(&depot);
		return result;
	}

	*depot_ptr = depot;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_slab_depot(struct slab_depot **depot_ptr)
{
	struct slab_depot *depot = *depot_ptr;
	if (depot == NULL) {
		return;
	}

	abandon_new_slabs(depot);

	zone_count_t zone = 0;
	for (zone = 0; zone < depot->zone_count; zone++) {
		free_block_allocator(&depot->allocators[zone]);
	}

	if (depot->slabs != NULL) {
		slab_count_t i;
		for (i = 0; i < depot->slab_count; i++) {
			free_slab(&depot->slabs[i]);
		}
	}

	FREE(depot->slabs);
	free_action_manager(&depot->action_manager);
	free_slab_summary(&depot->slab_summary);
	FREE(depot);
	*depot_ptr = NULL;
}

/**********************************************************************/
struct slab_depot_state_2_0 record_slab_depot(const struct slab_depot *depot)
{
	/*
	 * If this depot is currently using 0 zones, it must have been
	 * synchronously loaded by a tool and is now being saved. We
	 * did not load and combine the slab summary, so we still need
	 * to do that next time we load with the old zone count rather
	 * than 0.
	 */
	zone_count_t zones_to_record = depot->zone_count;
	if (depot->zone_count == 0) {
		zones_to_record = depot->old_zone_count;
	}

	struct slab_depot_state_2_0 state = {
		.slab_config = depot->slab_config,
		.first_block = depot->first_block,
		.last_block = depot->last_block,
		.zone_count = zones_to_record,
	};

	return state;
}

/**********************************************************************/
int allocate_slab_ref_counts(struct slab_depot *depot)
{
	struct slab_iterator iterator = get_slab_iterator(depot);
	while (has_next_slab(&iterator)) {
		int result = allocate_ref_counts_for_slab(next_slab(&iterator));
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
struct block_allocator *get_block_allocator_for_zone(struct slab_depot *depot,
						     zone_count_t zone_number)
{
	return depot->allocators[zone_number];
}

/**********************************************************************/
int get_slab_number(const struct slab_depot *depot,
		    physical_block_number_t pbn,
		    slab_count_t *slab_number_ptr)
{
	if (pbn < depot->first_block) {
		return VDO_OUT_OF_RANGE;
	}

	slab_count_t slab_number =
		(pbn - depot->first_block) >> depot->slab_size_shift;
	if (slab_number >= depot->slab_count) {
		return VDO_OUT_OF_RANGE;
	}

	*slab_number_ptr = slab_number;
	return VDO_SUCCESS;
}

/**********************************************************************/
struct vdo_slab *get_slab(const struct slab_depot *depot,
			  physical_block_number_t pbn)
{
	if (pbn == ZERO_BLOCK) {
		return NULL;
	}

	slab_count_t slab_number;
	int result = get_slab_number(depot, pbn, &slab_number);
	if (result != VDO_SUCCESS) {
		enter_read_only_mode(depot->read_only_notifier, result);
		return NULL;
	}

	return depot->slabs[slab_number];
}

/**********************************************************************/
struct slab_journal *get_slab_journal(const struct slab_depot *depot,
				      physical_block_number_t pbn)
{
	struct vdo_slab *slab = get_slab(depot, pbn);
	return ((slab != NULL) ? slab->journal : NULL);
}

/**********************************************************************/
uint8_t get_increment_limit(struct slab_depot *depot,
			    physical_block_number_t pbn)
{
	struct vdo_slab *slab = get_slab(depot, pbn);
	if ((slab == NULL) || is_unrecovered_slab(slab)) {
		return 0;
	}

	return get_available_references(slab->reference_counts, pbn);
}

/**********************************************************************/
bool is_physical_data_block(const struct slab_depot *depot,
			    physical_block_number_t pbn)
{
	if (pbn == ZERO_BLOCK) {
		return true;
	}

	slab_count_t slab_number;
	if (get_slab_number(depot, pbn, &slab_number) != VDO_SUCCESS) {
		return false;
	}

	slab_block_number sbn;
	int result = slab_block_number_from_pbn(depot->slabs[slab_number],
						pbn, &sbn);
	return (result == VDO_SUCCESS);
}

/**********************************************************************/
block_count_t get_depot_allocated_blocks(const struct slab_depot *depot)
{
	block_count_t total = 0;
	zone_count_t zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		// The allocators are responsible for thread safety.
		total += get_allocated_blocks(depot->allocators[zone]);
	}
	return total;
}

/**********************************************************************/
block_count_t get_depot_data_blocks(const struct slab_depot *depot)
{
	// XXX This needs to be thread safe, but resize changes the slab count.
	// It does so on the admin thread (our usual caller), so it's usually
	// safe.
	return (depot->slab_count * depot->slab_config.data_blocks);
}

/**********************************************************************/
block_count_t get_depot_free_blocks(const struct slab_depot *depot)
{
	/*
	 * We can't ever shrink a volume except when resize fails, and we can't
	 * allocate from the new slabs until after the resize succeeds, so by
	 * getting the number of allocated blocks first, we ensure the allocated
	 * count is always less than the capacity. Doing it in the other order
	 * on a full volume could lose a race with a sucessful resize, resulting
	 * in a nonsensical negative/underflow result.
	 */
	block_count_t allocated = get_depot_allocated_blocks(depot);
	smp_mb();
	return (get_depot_data_blocks(depot) - allocated);
}

/**********************************************************************/
slab_count_t get_depot_slab_count(const struct slab_depot *depot)
{
	return depot->slab_count;
}

/**********************************************************************/
slab_count_t get_depot_unrecovered_slab_count(const struct slab_depot *depot)
{
	slab_count_t total = 0;
	zone_count_t zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		// The allocators are responsible for thread safety.
		total += get_unrecovered_slab_count(depot->allocators[zone]);
	}
	return total;
}

/**
 * The preamble of a load operation which loads the slab summary.
 *
 * <p>Implements ActionPreamble.
 **/
static void start_depot_load(void *context, struct vdo_completion *parent)
{
	struct slab_depot *depot = context;
	load_slab_summary(depot->slab_summary,
			  get_current_manager_operation(depot->action_manager),
			  depot->old_zone_count,
			  parent);
}

/**********************************************************************/
void load_slab_depot(struct slab_depot *depot,
		     AdminStateCode operation,
		     struct vdo_completion *parent,
		     void *context)
{
	if (assert_load_operation(operation, parent)) {
		schedule_operation_with_context(depot->action_manager,
						operation,
						start_depot_load,
						load_block_allocator,
						NULL,
						context,
						parent);
	}
}

/**********************************************************************/
void prepare_to_allocate(struct slab_depot *depot,
			 slab_depot_load_type load_type,
			 struct vdo_completion *parent)
{
	depot->load_type = load_type;
	atomic_set(&depot->zones_to_scrub, depot->zone_count);
	schedule_action(depot->action_manager,
			NULL,
			prepare_allocator_to_allocate,
			NULL,
			parent);
}

/**********************************************************************/
void update_slab_depot_size(struct slab_depot *depot)
{
	depot->last_block = depot->new_last_block;
}

/**********************************************************************/
int prepare_to_grow_slab_depot(struct slab_depot *depot, block_count_t new_size)
{
	if ((new_size >> depot->slab_size_shift) <= depot->slab_count) {
		return VDO_INCREMENT_TOO_SMALL;
	}

	// Generate the depot configuration for the new block count.
	struct slab_depot_state_2_0 new_state;
	int result = configure_slab_depot(new_size,
					  depot->first_block,
					  depot->slab_config,
					  depot->zone_count,
					  &new_state);
	if (result != VDO_SUCCESS) {
		return result;
	}

	slab_count_t new_slab_count =
		compute_slab_count(depot->first_block, new_state.last_block,
				   depot->slab_size_shift);
	if (new_slab_count <= depot->slab_count) {
		return log_error_strerror(VDO_INCREMENT_TOO_SMALL,
					  "Depot can only grow");
	}
	if (new_slab_count == depot->new_slab_count) {
		// Check it out, we've already got all the new slabs allocated!
		return VDO_SUCCESS;
	}

	abandon_new_slabs(depot);
	result = allocate_slabs(depot, new_slab_count);
	if (result != VDO_SUCCESS) {
		abandon_new_slabs(depot);
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
 * <p>Implements ActionConclusion.
 **/
static int finish_registration(void *context)
{
	struct slab_depot *depot = context;
	depot->slab_count = depot->new_slab_count;
	FREE(depot->slabs);
	depot->slabs = depot->new_slabs;
	depot->new_slabs = NULL;
	depot->new_slab_count = 0;
	return VDO_SUCCESS;
}

/**********************************************************************/
void use_new_slabs(struct slab_depot *depot, struct vdo_completion *parent)
{
	ASSERT_LOG_ONLY(depot->new_slabs != NULL, "Must have new slabs to use");
	schedule_operation(depot->action_manager,
			   ADMIN_STATE_SUSPENDED_OPERATION,
			   NULL,
			   register_new_slabs_for_allocator,
			   finish_registration,
			   parent);
}

/**********************************************************************/
void drain_slab_depot(struct slab_depot *depot,
		      AdminStateCode operation,
		      struct vdo_completion *parent)
{
	schedule_operation(depot->action_manager,
			   operation,
			   NULL,
			   drain_block_allocator,
			   NULL,
			   parent);
}

/**********************************************************************/
void resume_slab_depot(struct slab_depot *depot, struct vdo_completion *parent)
{
	if (is_read_only(depot->read_only_notifier)) {
		finish_completion(parent, VDO_READ_ONLY);
		return;
	}

	schedule_operation(depot->action_manager,
			   ADMIN_STATE_RESUMING,
			   NULL,
			   resume_block_allocator,
			   NULL,
			   parent);
}

/**********************************************************************/
void
commit_oldest_slab_journal_tail_blocks(struct slab_depot *depot,
				       sequence_number_t recovery_block_number)
{
	if (depot == NULL) {
		return;
	}

	depot->new_release_request = recovery_block_number;
	schedule_default_action(depot->action_manager);
}

/**********************************************************************/
const struct slab_config *get_slab_config(const struct slab_depot *depot)
{
	return &depot->slab_config;
}

/**********************************************************************/
struct slab_summary *get_slab_summary(const struct slab_depot *depot)
{
	return depot->slab_summary;
}

/**********************************************************************/
struct slab_summary_zone *
get_slab_summary_for_zone(const struct slab_depot *depot, zone_count_t zone)
{
	if (depot->slab_summary == NULL) {
		return NULL;
	}

	return get_summary_for_zone(depot->slab_summary, zone);
}

/**********************************************************************/
void scrub_all_unrecovered_slabs(struct slab_depot *depot,
				 struct vdo_completion *parent)
{
	schedule_action(depot->action_manager,
			NULL,
			scrub_all_unrecovered_slabs_in_zone,
			NULL,
			parent);
}

/**********************************************************************/
void notify_zone_finished_scrubbing(struct vdo_completion *completion)
{
	struct slab_depot *depot = completion->parent;
	if (atomic_add_return(-1, &depot->zones_to_scrub) == 0) {
		// We're the last!
		if (compareAndSwap32(depot->vdo_state, VDO_RECOVERING,
				     VDO_DIRTY)) {
			log_info("Exiting recovery mode");
			return;
		}

		/*
		 * We must check the VDO state here and not the depot's
		 * read_only_notifier since the compare-swap-above could have
		 * failed due to a read-only entry which our own thread does not
		 * yet know about.
		 */
		if (atomicLoad32(depot->vdo_state) == VDO_DIRTY) {
			log_info("VDO commencing normal operation");
		}
	}
}

/**********************************************************************/
block_count_t get_new_depot_size(const struct slab_depot *depot)
{
	return (depot->new_slabs == NULL) ? 0 : depot->new_size;
}

/**********************************************************************/
bool are_equivalent_depots(struct slab_depot *depot_a,
			   struct slab_depot *depot_b)
{
	if ((depot_a->first_block != depot_b->first_block) ||
	    (depot_a->last_block != depot_b->last_block) ||
	    (depot_a->slab_count != depot_b->slab_count) ||
	    (depot_a->slab_size_shift != depot_b->slab_size_shift) ||
	    (get_depot_allocated_blocks(depot_a) !=
	     get_depot_allocated_blocks(depot_b))) {
		return false;
	}

	size_t i;
	for (i = 0; i < depot_a->slab_count; i++) {
		struct vdo_slab *slab_a = depot_a->slabs[i];
		struct vdo_slab *slab_b = depot_b->slabs[i];
		if ((slab_a->start != slab_b->start) ||
		    (slab_a->end != slab_b->end) ||
		    !are_equivalent_reference_counters(slab_a->reference_counts,
						       slab_b->reference_counts)) {
			return false;
		}
	}

	return true;
}

/**********************************************************************/
void allocate_from_last_slab(struct slab_depot *depot)
{
	zone_count_t zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		allocate_from_allocator_last_slab(depot->allocators[zone]);
	}
}

/**********************************************************************/
struct block_allocator_statistics
get_depot_block_allocator_statistics(const struct slab_depot *depot)
{
	struct block_allocator_statistics totals;
	memset(&totals, 0, sizeof(totals));

	zone_count_t zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		struct block_allocator *allocator = depot->allocators[zone];
		struct block_allocator_statistics stats =
			get_block_allocator_statistics(allocator);
		totals.slab_count += stats.slab_count;
		totals.slabs_opened += stats.slabs_opened;
		totals.slabs_reopened += stats.slabs_reopened;
	}

	return totals;
}

/**********************************************************************/
struct ref_counts_statistics
get_depot_ref_counts_statistics(const struct slab_depot *depot)
{
	struct ref_counts_statistics depot_stats;
	memset(&depot_stats, 0, sizeof(depot_stats));

	zone_count_t zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		struct block_allocator *allocator = depot->allocators[zone];
		struct ref_counts_statistics stats =
			get_ref_counts_statistics(allocator);
		depot_stats.blocks_written += stats.blocks_written;
	}

	return depot_stats;
}

/**********************************************************************/
struct slab_journal_statistics
get_depot_slab_journal_statistics(const struct slab_depot *depot)
{
	struct slab_journal_statistics depot_stats;
	memset(&depot_stats, 0, sizeof(depot_stats));

	zone_count_t zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		struct block_allocator *allocator = depot->allocators[zone];
		struct slab_journal_statistics stats =
			get_slab_journal_statistics(allocator);
		depot_stats.disk_full_count += stats.disk_full_count;
		depot_stats.flush_count += stats.flush_count;
		depot_stats.blocked_count += stats.blocked_count;
		depot_stats.blocks_written += stats.blocks_written;
		depot_stats.tail_busy_count += stats.tail_busy_count;
	}

	return depot_stats;
}

/**********************************************************************/
void dump_slab_depot(const struct slab_depot *depot)
{
	log_info("vdo slab depot");
	log_info("  zone_count=%u old_zone_count=%u slabCount=%u active_release_request=%llu new_release_request=%llu",
		 (unsigned int) depot->zone_count,
		 (unsigned int) depot->old_zone_count,
		 depot->slab_count,
		 depot->active_release_request,
		 depot->new_release_request);
}
