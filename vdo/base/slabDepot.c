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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepot.c#53 $
 */

#include "slabDepot.h"

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
#include "slabDepotInternals.h"
#include "slabJournal.h"
#include "slabIterator.h"
#include "slabSummary.h"
#include "threadConfig.h"
#include "types.h"
#include "vdoState.h"

struct slab_depot_state_2_0 {
	struct slab_config slab_config;
	PhysicalBlockNumber first_block;
	PhysicalBlockNumber last_block;
	ZoneCount zone_count;
} __attribute__((packed));

static const struct header SLAB_DEPOT_HEADER_2_0 = {
	.id = SLAB_DEPOT,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct slab_depot_state_2_0),
};

/**
 * Compute the number of slabs a depot with given parameters would have.
 *
 * @param first_block      PBN of the first data block
 * @param last_block       PBN of the last data block
 * @param slab_size_shift  Exponent for the number of blocks per slab
 *
 * @return The number of slabs
 **/
__attribute__((warn_unused_result)) static SlabCount
compute_slab_count(PhysicalBlockNumber first_block,
		   PhysicalBlockNumber last_block,
		   unsigned int slab_size_shift)
{
	BlockCount data_blocks = last_block - first_block;
	return (SlabCount)(data_blocks >> slab_size_shift);
}

/**********************************************************************/
SlabCount calculate_slab_count(struct slab_depot *depot)
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
static int allocate_slabs(struct slab_depot *depot, SlabCount slab_count)
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

	BlockCount slab_size = get_slab_config(depot)->slab_blocks;
	PhysicalBlockNumber slab_origin =
		depot->first_block + (depot->slab_count * slab_size);

	// The translation between allocator partition PBNs and layer PBNs.
	BlockCount translation = depot->origin - depot->first_block;
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
	SlabCount i;
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
static ThreadID get_allocator_thread_id(void *context, ZoneCount zone_number)
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
 * Schedule a tail block commit if necessary.
 *
 * <p>Implements ActionScheduler,
 **/
static bool scheduleTailBlockCommit(void *context)
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
			       Nonce nonce,
			       const struct thread_config *thread_config,
			       BlockCount vio_pool_size,
			       PhysicalLayer *layer,
			       struct partition *summary_partition)
{
	/*
	 * If createVIO is NULL, the slab depot is only being used to format
	 * or audit the VDO. These only require the SuperBlock component, so we
	 * can just skip allocating all the memory needed for runtime
	 * components.
	 */
	if (layer->createMetadataVIO == NULL) {
		return VDO_SUCCESS;
	}

	int result = make_action_manager(depot->zone_count,
					 get_allocator_thread_id,
					 get_journal_zone_thread(thread_config),
					 depot,
					 scheduleTailBlockCommit,
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

	SlabCount slab_count = calculate_slab_count(depot);
	if (thread_config->physical_zone_count > slab_count) {
		return logErrorWithStringError(VDO_BAD_CONFIGURATION,
					       "%u physical zones exceeds slab count %u",
					       thread_config->physical_zone_count,
					       slab_count);
	}

	// Allocate the block allocators.
	ZoneCount zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		ThreadID threadID = get_physical_zone_thread(thread_config, zone);
		result = make_block_allocator(depot,
					      zone,
					      threadID,
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
	SlabCount i;
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

/**
 * Allocate a slab depot.
 *
 * @param [in]  state               The parameters for the new depot
 * @param [in]  thread_config       The thread config of the VDO
 * @param [in]  nonce               The nonce of the VDO
 * @param [in]  vio_pool_size       The size of the VIO pool
 * @param [in]  layer               The physical layer below this depot
 * @param [in]  summary_partition   The partition which holds the slab summary
 *                                  (if NULL, the depot is format-only)
 * @param [in]  read_only_notifier  The context for entering read-only mode
 * @param [in]  recovery_journal    The recovery journal of the VDO
 * @param [in]  vdo_state           A pointer to the VDO's atomic state
 * @param [out] depot_ptr           A pointer to hold the depot
 *
 * @return A success or error code
 **/
__attribute__((warn_unused_result)) static int
allocate_depot(const struct slab_depot_state_2_0 *state,
	       const struct thread_config *thread_config,
	       Nonce nonce,
	       BlockCount vio_pool_size,
	       PhysicalLayer *layer,
	       struct partition *summary_partition,
	       struct read_only_notifier *read_only_notifier,
	       struct recovery_journal *recovery_journal,
	       Atomic32 *vdo_state,
	       struct slab_depot **depot_ptr)
{
	// Calculate the bit shift for efficiently mapping block numbers to
	// slabs. Using a shift requires that the slab size be a power of two.
	BlockCount slab_size = state->slab_config.slab_blocks;
	if (!is_power_of_two(slab_size)) {
		return logErrorWithStringError(UDS_INVALID_ARGUMENT,
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

	depot->old_zone_count = state->zone_count;
	depot->zone_count = thread_config->physical_zone_count;
	depot->slab_config = state->slab_config;
	depot->read_only_notifier = read_only_notifier;
	depot->first_block = state->first_block;
	depot->last_block = state->last_block;
	depot->slab_size_shift = slab_size_shift;
	depot->journal = recovery_journal;
	depot->vdo_state = vdo_state;

	result = allocate_components(depot,
				     nonce,
				     thread_config,
				     vio_pool_size,
				     layer,
				     summary_partition);
	if (result != VDO_SUCCESS) {
		free_slab_depot(&depot);
		return result;
	}

	*depot_ptr = depot;
	return VDO_SUCCESS;
}

/**
 * Configure the slab_depot for the specified storage capacity, finding the
 * number of data blocks that will fit and still leave room for the depot
 * metadata, then return the saved state for that configuration.
 *
 * @param [in]  block_count  The number of blocks in the underlying storage
 * @param [in]  first_block  The number of the first block that may be allocated
 * @param [in]  slab_config  The configuration of a single slab
 * @param [in]  zone_count   The number of zones the depot will use
 * @param [out] state        The state structure to be configured
 *
 * @return VDO_SUCCESS or an error code
 **/
static int configure_state(BlockCount block_count,
			   PhysicalBlockNumber first_block,
			   struct slab_config slab_config,
			   ZoneCount zone_count,
			   struct slab_depot_state_2_0 *state)
{
	BlockCount slab_size = slab_config.slab_blocks;
	logDebug("slabDepot configure_state(block_count=%" PRIu64
		 ", first_block=%llu, slab_size=%" PRIu64
		 ", zone_count=%u)",
		 block_count,
		 first_block,
		 slab_size,
		 zone_count);

	// We do not allow runt slabs, so we waste up to a slab's worth.
	size_t slab_count = (block_count / slab_size);
	if (slab_count == 0) {
		return VDO_NO_SPACE;
	}

	if (slab_count > MAX_SLABS) {
		return VDO_TOO_MANY_SLABS;
	}

	BlockCount total_slab_blocks = slab_count * slab_config.slab_blocks;
	BlockCount total_data_blocks = slab_count * slab_config.data_blocks;
	PhysicalBlockNumber last_block = first_block + total_slab_blocks;

	*state = (struct slab_depot_state_2_0) {
		.slab_config = slab_config,
		.first_block = first_block,
		.last_block = last_block,
		.zone_count = zone_count,
	};

	logDebug("slab_depot last_block=%llu, total_data_blocks=%" PRIu64
		 ", slab_count=%zu, left_over=%llu",
		 last_block,
		 total_data_blocks,
		 slab_count,
		 block_count - (last_block - first_block));

	return VDO_SUCCESS;
}

/**********************************************************************/
int make_slab_depot(BlockCount block_count,
		    PhysicalBlockNumber first_block,
		    struct slab_config slab_config,
		    const struct thread_config *thread_config,
		    Nonce nonce,
		    BlockCount vio_pool_size,
		    PhysicalLayer *layer,
		    struct partition *summary_partition,
		    struct read_only_notifier *read_only_notifier,
		    struct recovery_journal *recovery_journal,
		    Atomic32 *vdo_state,
		    struct slab_depot **depot_ptr)
{
	struct slab_depot_state_2_0 state;
	int result =
		configure_state(block_count, first_block, slab_config, 0,
				&state);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct slab_depot *depot = NULL;
	result = allocate_depot(&state,
				thread_config,
				nonce,
				vio_pool_size,
				layer,
				summary_partition,
				read_only_notifier,
				recovery_journal,
				vdo_state,
				&depot);
	if (result != VDO_SUCCESS) {
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

	ZoneCount zone = 0;
	for (zone = 0; zone < depot->zone_count; zone++) {
		free_block_allocator(&depot->allocators[zone]);
	}

	if (depot->slabs != NULL) {
		SlabCount i;
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
size_t get_slab_depot_encoded_size(void)
{
	return ENCODED_HEADER_SIZE + sizeof(struct slab_depot_state_2_0);
}

/**
 * Decode a slab config from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The config structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decode_slab_config(Buffer *buffer, struct slab_config *config)
{
	BlockCount count;
	int result = getUInt64LEFromBuffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_blocks = count;

	result = getUInt64LEFromBuffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->data_blocks = count;

	result = getUInt64LEFromBuffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->reference_count_blocks = count;

	result = getUInt64LEFromBuffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_blocks = count;

	result = getUInt64LEFromBuffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_flushing_threshold = count;

	result = getUInt64LEFromBuffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_blocking_threshold = count;

	result = getUInt64LEFromBuffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_scrubbing_threshold = count;

	return UDS_SUCCESS;
}

/**
 * Encode a slab config into a buffer.
 *
 * @param config  The config structure to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int encode_slab_config(const struct slab_config *config, Buffer *buffer)
{
	int result = putUInt64LEIntoBuffer(buffer, config->slab_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, config->data_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, config->reference_count_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, config->slab_journal_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer,
				       config->slab_journal_flushing_threshold);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer,
				       config->slab_journal_blocking_threshold);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return putUInt64LEIntoBuffer(buffer,
				     config->slab_journal_scrubbing_threshold);
}

/**********************************************************************/
int encode_slab_depot(const struct slab_depot *depot, Buffer *buffer)
{
	int result = encode_header(&SLAB_DEPOT_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t initial_length = contentLength(buffer);

	result = encode_slab_config(&depot->slab_config, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, depot->first_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, depot->last_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * If this depot is currently using 0 zones, it must have been
	 * synchronously loaded by a tool and is now being saved. We
	 * did not load and combine the slab summary, so we still need
	 * to do that next time we load with the old zone count rather
	 * than 0.
	 */
	ZoneCount zones_to_record = depot->zone_count;
	if (depot->zone_count == 0) {
		zones_to_record = depot->old_zone_count;
	}
	result = putByte(buffer, zones_to_record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t encoded_size = contentLength(buffer) - initial_length;
	return ASSERT(SLAB_DEPOT_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}

/**
 * Decode slab depot component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decode_slab_depot_state_2_0(Buffer *buffer,
				       struct slab_depot_state_2_0 *state)
{
	size_t initial_length = contentLength(buffer);

	int result = decode_slab_config(buffer, &state->slab_config);
	if (result != UDS_SUCCESS) {
		return result;
	}

	PhysicalBlockNumber first_block;
	result = getUInt64LEFromBuffer(buffer, &first_block);
	if (result != UDS_SUCCESS) {
		return result;
	}
	state->first_block = first_block;

	PhysicalBlockNumber last_block;
	result = getUInt64LEFromBuffer(buffer, &last_block);
	if (result != UDS_SUCCESS) {
		return result;
	}
	state->last_block = last_block;

	result = getByte(buffer, &state->zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t decoded_size = initial_length - contentLength(buffer);
	return ASSERT(SLAB_DEPOT_HEADER_2_0.size == decoded_size,
		      "decoded slab depot component size must match header size");
}

/**********************************************************************/
int decode_slab_depot(Buffer *buffer,
		      const struct thread_config *thread_config,
		      Nonce nonce,
		      PhysicalLayer *layer,
		      struct partition *summary_partition,
		      struct read_only_notifier *read_only_notifier,
		      struct recovery_journal *recovery_journal,
		      Atomic32 *vdo_state,
		      struct slab_depot **depot_ptr)
{
	struct header header;
	int result = decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_header(&SLAB_DEPOT_HEADER_2_0, &header, true,
				 __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct slab_depot_state_2_0 state;
	result = decode_slab_depot_state_2_0(buffer, &state);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return allocate_depot(&state,
			      thread_config,
			      nonce,
			      VIO_POOL_SIZE,
			      layer,
			      summary_partition,
			      read_only_notifier,
			      recovery_journal,
			      vdo_state,
			      depot_ptr);
}

/**********************************************************************/
int decode_sodium_slab_depot(Buffer *buffer,
			     const struct thread_config *thread_config,
			     Nonce nonce,
			     PhysicalLayer *layer,
			     struct partition *summary_partition,
			     struct read_only_notifier *read_only_notifier,
			     struct recovery_journal *recovery_journal,
			     struct slab_depot **depot_ptr)
{
	// Sodium uses version 2.0 of the slab depot state.
	return decode_slab_depot(buffer,
				 thread_config,
				 nonce,
				 layer,
				 summary_partition,
				 read_only_notifier,
				 recovery_journal,
				 NULL,
				 depot_ptr);
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
						     ZoneCount zone_number)
{
	return depot->allocators[zone_number];
}

/**********************************************************************/
int get_slab_number(const struct slab_depot *depot,
		    PhysicalBlockNumber pbn,
		    SlabCount *slab_number_ptr)
{
	if (pbn < depot->first_block) {
		return VDO_OUT_OF_RANGE;
	}

	SlabCount slab_number =
		(pbn - depot->first_block) >> depot->slab_size_shift;
	if (slab_number >= depot->slab_count) {
		return VDO_OUT_OF_RANGE;
	}

	*slab_number_ptr = slab_number;
	return VDO_SUCCESS;
}

/**********************************************************************/
struct vdo_slab *get_slab(const struct slab_depot *depot,
			  PhysicalBlockNumber pbn)
{
	if (pbn == ZERO_BLOCK) {
		return NULL;
	}

	SlabCount slab_number;
	int result = get_slab_number(depot, pbn, &slab_number);
	if (result != VDO_SUCCESS) {
		enter_read_only_mode(depot->read_only_notifier, result);
		return NULL;
	}

	return depot->slabs[slab_number];
}

/**********************************************************************/
struct slab_journal *get_slab_journal(const struct slab_depot *depot,
				      PhysicalBlockNumber pbn)
{
	struct vdo_slab *slab = get_slab(depot, pbn);
	return ((slab != NULL) ? slab->journal : NULL);
}

/**********************************************************************/
uint8_t get_increment_limit(struct slab_depot *depot, PhysicalBlockNumber pbn)
{
	struct vdo_slab *slab = get_slab(depot, pbn);
	if ((slab == NULL) || is_unrecovered_slab(slab)) {
		return 0;
	}

	return get_available_references(slab->reference_counts, pbn);
}

/**********************************************************************/
bool is_physical_data_block(const struct slab_depot *depot,
			    PhysicalBlockNumber pbn)
{
	if (pbn == ZERO_BLOCK) {
		return true;
	}

	SlabCount slab_number;
	if (get_slab_number(depot, pbn, &slab_number) != VDO_SUCCESS) {
		return false;
	}

	slab_block_number sbn;
	int result = slab_block_number_from_pbn(depot->slabs[slab_number],
						pbn, &sbn);
	return (result == VDO_SUCCESS);
}

/**********************************************************************/
BlockCount get_depot_allocated_blocks(const struct slab_depot *depot)
{
	BlockCount total = 0;
	ZoneCount zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		// The allocators are responsible for thread safety.
		total += get_allocated_blocks(depot->allocators[zone]);
	}
	return total;
}

/**********************************************************************/
BlockCount get_depot_data_blocks(const struct slab_depot *depot)
{
	// XXX This needs to be thread safe, but resize changes the slab count.
	// It does so on the admin thread (our usual caller), so it's usually
	// safe.
	return (depot->slab_count * depot->slab_config.data_blocks);
}

/**********************************************************************/
BlockCount get_depot_free_blocks(const struct slab_depot *depot)
{
	/*
	 * We can't ever shrink a volume except when resize fails, and we can't
	 * allocate from the new slabs until after the resize succeeds, so by
	 * getting the number of allocated blocks first, we ensure the allocated
	 * count is always less than the capacity. Doing it in the other order
	 * on a full volume could lose a race with a sucessful resize, resulting
	 * in a nonsensical negative/underflow result.
	 */
	BlockCount allocated = get_depot_allocated_blocks(depot);
	memoryFence();
	return (get_depot_data_blocks(depot) - allocated);
}

/**********************************************************************/
SlabCount get_depot_slab_count(const struct slab_depot *depot)
{
	return depot->slab_count;
}

/**********************************************************************/
SlabCount get_depot_unrecovered_slab_count(const struct slab_depot *depot)
{
	SlabCount total = 0;
	ZoneCount zone;
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
	atomicStore32(&depot->zones_to_scrub, depot->zone_count);
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
int prepare_to_grow_slab_depot(struct slab_depot *depot, BlockCount new_size)
{
	if ((new_size >> depot->slab_size_shift) <= depot->slab_count) {
		return VDO_INCREMENT_TOO_SMALL;
	}

	// Generate the depot configuration for the new block count.
	struct slab_depot_state_2_0 new_state;
	int result = configure_state(new_size,
				    depot->first_block,
				    depot->slab_config,
				    depot->zone_count,
				    &new_state);
	if (result != VDO_SUCCESS) {
		return result;
	}

	SlabCount new_slab_count = compute_slab_count(depot->first_block,
						      new_state.last_block,
						      depot->slab_size_shift);
	if (new_slab_count <= depot->slab_count) {
		return logErrorWithStringError(VDO_INCREMENT_TOO_SMALL,
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
				       SequenceNumber recovery_block_number)
{
	if (depot == NULL) {
		return;
	}

	depot->new_release_request = recovery_block_number;
	scheduleTailBlockCommit(depot);
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
get_slab_summary_for_zone(const struct slab_depot *depot, ZoneCount zone)
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
	if (atomicAdd32(&depot->zones_to_scrub, -1) == 0) {
		// We're the last!
		if (compareAndSwap32(depot->vdo_state, VDO_RECOVERING,
				     VDO_DIRTY)) {
			logInfo("Exiting recovery mode");
			return;
		}

		/*
		 * We must check the VDO state here and not the depot's
		 * read_only_notifier since the compare-swap-above could have
		 * failed due to a read-only entry which our own thread does not
		 * yet know about.
		 */
		if (atomicLoad32(depot->vdo_state) == VDO_DIRTY) {
			logInfo("VDO commencing normal operation");
		}
	}
}

/**********************************************************************/
bool has_unrecovered_slabs(struct slab_depot *depot)
{
	return (atomicLoad32(&depot->zones_to_scrub) > 0);
}

/**********************************************************************/
BlockCount get_new_depot_size(const struct slab_depot *depot)
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
	ZoneCount zone;
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

	ZoneCount zone;
	for (zone = 0; zone < depot->zone_count; zone++) {
		struct block_allocator *allocator = depot->allocators[zone];
		struct block_allocator_statistics stats =
			get_block_allocator_statistics(allocator);
		totals.slabCount += stats.slabCount;
		totals.slabsOpened += stats.slabsOpened;
		totals.slabsReopened += stats.slabsReopened;
	}

	return totals;
}

/**********************************************************************/
struct ref_counts_statistics
get_depot_ref_counts_statistics(const struct slab_depot *depot)
{
	struct ref_counts_statistics depot_stats;
	memset(&depot_stats, 0, sizeof(depot_stats));

	ZoneCount zone;
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

	ZoneCount zone;
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
	logInfo("vdo slab depot");
	logInfo("  zone_count=%u old_zone_count=%u slabCount=%" PRIu32
		" active_release_request=%" PRIu64
		" new_release_request=%llu",
		(unsigned int) depot->zone_count,
		(unsigned int) depot->old_zone_count,
		depot->slab_count,
		depot->active_release_request,
		depot->new_release_request);
}
