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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo.c#47 $
 */

/*
 * This file contains the main entry points for normal operations on a vdo as
 * well as functions for constructing and destroying vdo instances (in memory).
 */

#include "vdoInternal.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "extent.h"
#include "hashZone.h"
#include "header.h"
#include "logicalZone.h"
#include "numUtils.h"
#include "packer.h"
#include "physicalZone.h"
#include "readOnlyNotifier.h"
#include "recoveryJournal.h"
#include "releaseVersions.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "statistics.h"
#include "statusCodes.h"
#include "threadConfig.h"
#include "vdoLayout.h"
#include "vioWrite.h"
#include "volumeGeometry.h"

/**
 * The master version of the on-disk format of a VDO. This should be
 * incremented any time the on-disk representation of any VDO structure
 * changes. Changes which require only online upgrade steps should increment
 * the minor version. Changes which require an offline upgrade or which can not
 * be upgraded to at all should increment the major version and set the minor
 * version to 0.
 **/
static const struct version_number VDO_MASTER_VERSION_67_0 = {
	.major_version = 67,
	.minor_version = 0,
};

/**
 * The current version for the data encoded in the super block. This must
 * be changed any time there is a change to encoding of the component data
 * of any VDO component.
 **/
static const struct version_number VDO_COMPONENT_DATA_41_0 = {
	.major_version = 41,
	.minor_version = 0,
};

/**
 * This is the structure that captures the vdo fields saved as a SuperBlock
 * component.
 **/
struct vdo_component_41_0 {
	VDOState state;
	uint64_t complete_recoveries;
	uint64_t read_only_recoveries;
	VDOConfig config;
	Nonce nonce;
} __attribute__((packed));

/**********************************************************************/
int allocate_vdo(PhysicalLayer *layer, struct vdo **vdo_ptr)
{
	int result = register_status_codes();
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct vdo *vdo;
	result = ALLOCATE(1, struct vdo, __func__, &vdo);
	if (result != UDS_SUCCESS) {
		return result;
	}

	vdo->layer = layer;
	if (layer->createEnqueueable != NULL) {
		result = initialize_admin_completion(vdo,
						     &vdo->admin_completion);
		if (result != VDO_SUCCESS) {
			free_vdo(&vdo);
			return result;
		}
	}

	*vdo_ptr = vdo;
	return VDO_SUCCESS;
}

/**********************************************************************/
int make_vdo(PhysicalLayer *layer, struct vdo **vdo_ptr)
{
	struct vdo *vdo;
	int result = allocate_vdo(layer, &vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = makeZeroThreadConfig(&vdo->load_config.threadConfig);
	if (result != VDO_SUCCESS) {
		free_vdo(&vdo);
		return result;
	}

	*vdo_ptr = vdo;
	return VDO_SUCCESS;
}

/**********************************************************************/
void destroy_vdo(struct vdo *vdo)
{
	free_flusher(&vdo->flusher);
	free_packer(&vdo->packer);
	free_recovery_journal(&vdo->recovery_journal);
	free_slab_depot(&vdo->depot);
	free_vdo_layout(&vdo->layout);
	free_super_block(&vdo->super_block);
	free_block_map(&vdo->block_map);

	const struct thread_config *thread_config = get_thread_config(vdo);
	if (vdo->hash_zones != NULL) {
		ZoneCount zone;
		for (zone = 0; zone < thread_config->hashZoneCount; zone++) {
			free_hash_zone(&vdo->hash_zones[zone]);
		}
	}
	FREE(vdo->hash_zones);
	vdo->hash_zones = NULL;

	free_logical_zones(&vdo->logical_zones);

	if (vdo->physical_zones != NULL) {
		ZoneCount zone;
		for (zone = 0; zone < thread_config->physicalZoneCount; zone++) {
			free_physical_zone(&vdo->physical_zones[zone]);
		}
	}
	FREE(vdo->physical_zones);
	vdo->physical_zones = NULL;

	uninitialize_admin_completion(&vdo->admin_completion);
	free_read_only_notifier(&vdo->read_only_notifier);
	freeThreadConfig(&vdo->load_config.threadConfig);
}

/**********************************************************************/
void free_vdo(struct vdo **vdo_ptr)
{
	if (*vdo_ptr == NULL) {
		return;
	}

	destroy_vdo(*vdo_ptr);
	FREE(*vdo_ptr);
	*vdo_ptr = NULL;
}

/**********************************************************************/
VDOState get_vdo_state(const struct vdo *vdo)
{
	return atomicLoad32(&vdo->state);
}

/**********************************************************************/
void set_vdo_state(struct vdo *vdo, VDOState state)
{
	atomicStore32(&vdo->state, state);
}

/**********************************************************************/
size_t get_component_data_size(struct vdo *vdo)
{
	return (sizeof(struct version_number) + sizeof(struct version_number) +
		sizeof(struct vdo_component_41_0) +
		get_vdo_layout_encoded_size(vdo->layout) +
		get_recovery_journal_encoded_size() +
		get_slab_depot_encoded_size() + get_block_map_encoded_size());
}

/**
 * Encode the vdo master version.
 *
 * @param buffer  The buffer in which to encode the version
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
encode_master_version(Buffer *buffer)
{
	return encode_version_number(VDO_MASTER_VERSION_67_0, buffer);
}

/**
 * Encode a VDOConfig structure into a buffer.
 *
 * @param config  The config structure to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
encode_vdo_config(const VDOConfig *config, Buffer *buffer)
{
	int result = putUInt64LEIntoBuffer(buffer, config->logicalBlocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, config->physicalBlocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, config->slabSize);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, config->recoveryJournalSize);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return putUInt64LEIntoBuffer(buffer, config->slabJournalBlocks);
}

/**
 * Encode the component data for the vdo itself.
 *
 * @param vdo     The vdo to encode
 * @param buffer  The buffer in which to encode the vdo
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
encode_vdo_component(const struct vdo *vdo, Buffer *buffer)
{
	int result = encode_version_number(VDO_COMPONENT_DATA_41_0, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	size_t initial_length = contentLength(buffer);

	result = putUInt32LEIntoBuffer(buffer, get_vdo_state(vdo));
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, vdo->complete_recoveries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, vdo->read_only_recoveries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_vdo_config(&vdo->config, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, vdo->nonce);
	if (result != VDO_SUCCESS) {
		return result;
	}

	size_t encoded_size = contentLength(buffer) - initial_length;
	return ASSERT(encoded_size == sizeof(struct vdo_component_41_0),
		      "encoded VDO component size must match structure size");
}

/**********************************************************************/
static int encode_vdo(struct vdo *vdo)
{
	Buffer *buffer = get_component_buffer(vdo->super_block);
	int result = resetBufferEnd(buffer, 0);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_master_version(buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_vdo_component(vdo, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_vdo_layout(vdo->layout, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_recovery_journal(vdo->recovery_journal, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_slab_depot(vdo->depot, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_block_map(vdo->block_map, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	ASSERT_LOG_ONLY((contentLength(buffer) == get_component_data_size(vdo)),
			"All super block component data was encoded");
	return VDO_SUCCESS;
}

/**********************************************************************/
int save_vdo_components(struct vdo *vdo)
{
	int result = encode_vdo(vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return save_super_block(vdo->layer, vdo->super_block,
				get_first_block_offset(vdo));
}

/**********************************************************************/
void save_vdo_components_async(struct vdo *vdo, struct vdo_completion *parent)
{
	int result = encode_vdo(vdo);
	if (result != VDO_SUCCESS) {
		finish_completion(parent, result);
		return;
	}

	save_super_block_async(vdo->super_block, get_first_block_offset(vdo),
			       parent);
}

/**********************************************************************/
int save_reconfigured_vdo(struct vdo *vdo)
{
	Buffer *buffer = get_component_buffer(vdo->super_block);
	size_t components_size = contentLength(buffer);

	byte *components;
	int result = copyBytes(buffer, components_size, &components);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = resetBufferEnd(buffer, 0);
	if (result != VDO_SUCCESS) {
		FREE(components);
		return result;
	}

	result = encode_master_version(buffer);
	if (result != VDO_SUCCESS) {
		FREE(components);
		return result;
	}

	result = encode_vdo_component(vdo, buffer);
	if (result != VDO_SUCCESS) {
		FREE(components);
		return result;
	}

	result = putBytes(buffer, components_size, components);
	FREE(components);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return save_super_block(vdo->layer, vdo->super_block,
				get_first_block_offset(vdo));
}

/**********************************************************************/
int decode_vdo_version(struct vdo *vdo)
{
	return decode_version_number(get_component_buffer(vdo->super_block),
				     &vdo->load_version);
}

/**********************************************************************/
int validate_vdo_version(struct vdo *vdo)
{
	int result = decode_vdo_version(vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	ReleaseVersionNumber loaded_release_version =
		get_loaded_release_version(vdo->super_block);
	if (vdo->load_config.releaseVersion != loaded_release_version) {
		return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
					       "Geometry release version %" PRIu32 " does not match super block release version %" PRIu32,
					       vdo->load_config.releaseVersion,
					       loaded_release_version);
	}

	return validate_version(VDO_MASTER_VERSION_67_0, vdo->load_version,
				"master");
}

/**
 * Decode a VDOConfig structure from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The config structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result)) static int
decode_vdo_config(Buffer *buffer, VDOConfig *config)
{
	BlockCount logical_blocks;
	int result = getUInt64LEFromBuffer(buffer, &logical_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	BlockCount physical_blocks;
	result = getUInt64LEFromBuffer(buffer, &physical_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	BlockCount slab_size;
	result = getUInt64LEFromBuffer(buffer, &slab_size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	BlockCount recovery_journal_size;
	result = getUInt64LEFromBuffer(buffer, &recovery_journal_size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	BlockCount slab_journal_blocks;
	result = getUInt64LEFromBuffer(buffer, &slab_journal_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*config = (VDOConfig) {
		.logicalBlocks = logical_blocks,
		.physicalBlocks = physical_blocks,
		.slabSize = slab_size,
		.recoveryJournalSize = recovery_journal_size,
		.slabJournalBlocks = slab_journal_blocks,
	};
	return VDO_SUCCESS;
}

/**
 * Decode the version 41.0 component state for the vdo itself from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
decode_vdo_component_41_0(Buffer *buffer, struct vdo_component_41_0 *state)
{
	size_t initial_length = contentLength(buffer);

	VDOState vdo_state;
	int result = getUInt32LEFromBuffer(buffer, &vdo_state);
	if (result != VDO_SUCCESS) {
		return result;
	}

	uint64_t complete_recoveries;
	result = getUInt64LEFromBuffer(buffer, &complete_recoveries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	uint64_t read_only_recoveries;
	result = getUInt64LEFromBuffer(buffer, &read_only_recoveries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	VDOConfig config;
	result = decode_vdo_config(buffer, &config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	Nonce nonce;
	result = getUInt64LEFromBuffer(buffer, &nonce);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*state = (struct vdo_component_41_0) {
		.state = vdo_state,
		.complete_recoveries = complete_recoveries,
		.read_only_recoveries = read_only_recoveries,
		.config = config,
		.nonce = nonce,
	};

	size_t decoded_size = initial_length - contentLength(buffer);
	return ASSERT(decoded_size == sizeof(struct vdo_component_41_0),
		      "decoded VDO component size must match structure size");
}

/**********************************************************************/
int decode_vdo_component(struct vdo *vdo)
{
	Buffer *buffer = get_component_buffer(vdo->super_block);

	struct version_number version;
	int result = decode_version_number(buffer, &version);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_version(version, VDO_COMPONENT_DATA_41_0,
				  "VDO component data");
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct vdo_component_41_0 component;
	result = decode_vdo_component_41_0(buffer, &component);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Copy the decoded component into the vdo structure.
	set_vdo_state(vdo, component.state);
	vdo->load_state = component.state;
	vdo->complete_recoveries = component.complete_recoveries;
	vdo->read_only_recoveries = component.read_only_recoveries;
	vdo->config = component.config;
	vdo->nonce = component.nonce;
	return VDO_SUCCESS;
}

/**********************************************************************/
int validate_vdo_config(const VDOConfig *config,
			BlockCount block_count,
			bool require_logical)
{
	int result = ASSERT(config->slabSize > 0, "slab size unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(is_power_of_two(config->slabSize),
			"slab size must be a power of two");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->slabSize <= (1 << MAX_SLAB_BITS),
			"slab size must be less than or equal to 2^%d",
			MAX_SLAB_BITS);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result =
		ASSERT(config->slabJournalBlocks >= MINIMUM_SLAB_JOURNAL_BLOCKS,
		       "slab journal size meets minimum size");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->slabJournalBlocks <= config->slabSize,
			"slab journal size is within expected bound");
	if (result != UDS_SUCCESS) {
		return result;
	}

	SlabConfig slab_config;
	result = configure_slab(config->slabSize, config->slabJournalBlocks,
				&slab_config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ASSERT((slab_config.dataBlocks >= 1),
			"slab must be able to hold at least one block");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->physicalBlocks > 0,
			"physical blocks unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->physicalBlocks <= MAXIMUM_PHYSICAL_BLOCKS,
			"physical block count %llu exceeds maximum %llu",
			config->physicalBlocks,
			MAXIMUM_PHYSICAL_BLOCKS);
	if (result != UDS_SUCCESS) {
		return VDO_OUT_OF_RANGE;
	}

	// This can't check equality because FileLayer et al can only known
	// about the storage size, which may not match the super block size.
	if (block_count < config->physicalBlocks) {
		logError("A physical size of %llu blocks was specified, but that is smaller than the %llu blocks configured in the vdo super block",
			 block_count,
			 config->physicalBlocks);
		return VDO_PARAMETER_MISMATCH;
	}

	result = ASSERT(!require_logical || (config->logicalBlocks > 0),
			"logical blocks unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->logicalBlocks <= MAXIMUM_LOGICAL_BLOCKS,
			"logical blocks too large");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->recoveryJournalSize > 0,
			"recovery journal size unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(is_power_of_two(config->recoveryJournalSize),
			"recovery journal size must be a power of two");
	if (result != UDS_SUCCESS) {
		return result;
	}

	return result;
}

/**
 * Notify a vdo that it is going read-only. This will save the read-only state
 * to the super block.
 *
 * <p>Implements ReadOnlyNotification.
 *
 * @param listener  The vdo
 * @param parent    The completion to notify in order to acknowledge the
 *                  notification
 **/
static void notify_vdo_of_read_only_mode(void *listener,
				    struct vdo_completion *parent)
{
	struct vdo *vdo = listener;
	if (in_read_only_mode(vdo)) {
		complete_completion(parent);
	}

	set_vdo_state(vdo, VDO_READ_ONLY_MODE);
	save_vdo_components_async(vdo, parent);
}

/**********************************************************************/
int enable_read_only_entry(struct vdo *vdo)
{
	return register_read_only_listener(vdo->read_only_notifier,
					   vdo,
					   notify_vdo_of_read_only_mode,
					   getAdminThread(get_thread_config(vdo)));
}

/**********************************************************************/
bool in_read_only_mode(const struct vdo *vdo)
{
	return (get_vdo_state(vdo) == VDO_READ_ONLY_MODE);
}

/**********************************************************************/
bool was_new(const struct vdo *vdo)
{
	return (vdo->load_state == VDO_NEW);
}

/**********************************************************************/
bool requires_read_only_rebuild(const struct vdo *vdo)
{
	return ((vdo->load_state == VDO_FORCE_REBUILD) ||
		(vdo->load_state == VDO_REBUILD_FOR_UPGRADE));
}

/**********************************************************************/
bool requires_rebuild(const struct vdo *vdo)
{
	switch (get_vdo_state(vdo)) {
	case VDO_DIRTY:
	case VDO_FORCE_REBUILD:
	case VDO_REPLAYING:
	case VDO_REBUILD_FOR_UPGRADE:
		return true;

	default:
		return false;
	}
}

/**********************************************************************/
bool requires_recovery(const struct vdo *vdo)
{
	return ((vdo->load_state == VDO_DIRTY) ||
		(vdo->load_state == VDO_REPLAYING) ||
		(vdo->load_state == VDO_RECOVERING));
}

/**********************************************************************/
bool is_replaying(const struct vdo *vdo)
{
	return (get_vdo_state(vdo) == VDO_REPLAYING);
}

/**********************************************************************/
bool in_recovery_mode(const struct vdo *vdo)
{
	return (get_vdo_state(vdo) == VDO_RECOVERING);
}

/**********************************************************************/
void enter_recovery_mode(struct vdo *vdo)
{
	assert_on_admin_thread(vdo, __func__);

	if (in_read_only_mode(vdo)) {
		return;
	}

	logInfo("Entering recovery mode");
	set_vdo_state(vdo, VDO_RECOVERING);
}

/**********************************************************************/
void make_vdo_read_only(struct vdo *vdo, int error_code)
{
	enter_read_only_mode(vdo->read_only_notifier, error_code);
}

/**********************************************************************/
bool set_vdo_compressing(struct vdo *vdo, bool enable_compression)
{
	bool state_changed = compareAndSwapBool(&vdo->compressing,
						!enable_compression,
						enable_compression);
	if (state_changed && !enable_compression) {
		// Flushing the packer is asynchronous, but we don't care when
		// it finishes.
		flush_packer(vdo->packer);
	}

	logInfo("compression is %s",
		(enable_compression ? "enabled" : "disabled"));
	return (state_changed ? !enable_compression : enable_compression);
}

/**********************************************************************/
bool get_vdo_compressing(struct vdo *vdo)
{
	return atomicLoadBool(&vdo->compressing);
}

/**********************************************************************/
static size_t get_block_map_cache_size(const struct vdo *vdo)
{
	return ((size_t) vdo->load_config.cacheSize) * VDO_BLOCK_SIZE;
}

/**
 * Tally the hash lock statistics from all the hash zones.
 *
 * @param vdo  The vdo to query
 *
 * @return The sum of the hash lock statistics from all hash zones
 **/
static HashLockStatistics get_hash_lock_statistics(const struct vdo *vdo)
{
	HashLockStatistics totals;
	memset(&totals, 0, sizeof(totals));

	const struct thread_config *thread_config = get_thread_config(vdo);
	ZoneCount zone;
	for (zone = 0; zone < thread_config->hashZoneCount; zone++) {
		HashLockStatistics stats =
			get_hash_zone_statistics(vdo->hash_zones[zone]);
		totals.dedupeAdviceValid += stats.dedupeAdviceValid;
		totals.dedupeAdviceStale += stats.dedupeAdviceStale;
		totals.concurrentDataMatches += stats.concurrentDataMatches;
		totals.concurrentHashCollisions +=
			stats.concurrentHashCollisions;
	}

	return totals;
}

/**
 * Get the current error statistics from a vdo.
 *
 * @param vdo  The vdo to query
 *
 * @return a copy of the current vdo error counters
 **/
static ErrorStatistics get_vdo_error_statistics(const struct vdo *vdo)
{
	/*
	 * The error counts can be incremented from arbitrary threads and so
	 * must be incremented atomically, but they are just statistics with no
	 * semantics that could rely on memory order, so unfenced reads are
	 * sufficient.
	 */
	const struct atomic_error_statistics *atoms = &vdo->error_stats;
	return (ErrorStatistics) {
		.invalidAdvicePBNCount =
			relaxedLoad64(&atoms->invalidAdvicePBNCount),
		.noSpaceErrorCount = relaxedLoad64(&atoms->noSpaceErrorCount),
		.readOnlyErrorCount = relaxedLoad64(&atoms->readOnlyErrorCount),
	};
}

/**********************************************************************/
static const char *describe_write_policy(WritePolicy policy)
{
	switch (policy) {
	case WRITE_POLICY_ASYNC:
		return "async";
	case WRITE_POLICY_ASYNC_UNSAFE:
		return "async-unsafe";
	case WRITE_POLICY_SYNC:
		return "sync";
	default:
		return "unknown";
	}
}

/**********************************************************************/
void get_vdo_statistics(const struct vdo *vdo, VDOStatistics *stats)
{
	// These are immutable properties of the vdo object, so it is safe to
	// query them from any thread.
	struct recovery_journal *journal = vdo->recovery_journal;
	struct slab_depot *depot = vdo->depot;
	// XXX config.physicalBlocks is actually mutated during resize and is in
	// a packed structure, but resize runs on the admin thread so we're
	// usually OK.
	stats->version = STATISTICS_VERSION;
	stats->releaseVersion = CURRENT_RELEASE_VERSION_NUMBER;
	stats->logicalBlocks = vdo->config.logicalBlocks;
	stats->physicalBlocks = vdo->config.physicalBlocks;
	stats->blockSize = VDO_BLOCK_SIZE;
	stats->completeRecoveries = vdo->complete_recoveries;
	stats->readOnlyRecoveries = vdo->read_only_recoveries;
	stats->blockMapCacheSize = get_block_map_cache_size(vdo);
	snprintf(stats->writePolicy,
		 sizeof(stats->writePolicy),
		 "%s",
		 describe_write_policy(get_write_policy(vdo)));

	// The callees are responsible for thread-safety.
	stats->dataBlocksUsed = get_physical_blocks_allocated(vdo);
	stats->overheadBlocksUsed = get_physical_blocks_overhead(vdo);
	stats->logicalBlocksUsed = get_journal_logical_blocks_used(journal);
	stats->allocator = get_depot_block_allocator_statistics(depot);
	stats->journal = get_recovery_journal_statistics(journal);
	stats->packer = get_packer_statistics(vdo->packer);
	stats->slabJournal = get_depot_slab_journal_statistics(depot);
	stats->slabSummary =
		get_slab_summary_statistics(get_slab_summary(depot));
	stats->refCounts = get_depot_ref_counts_statistics(depot);
	stats->blockMap = get_block_map_statistics(vdo->block_map);
	stats->hashLock = get_hash_lock_statistics(vdo);
	stats->errors = get_vdo_error_statistics(vdo);
	SlabCount slab_total = get_depot_slab_count(depot);
	stats->recoveryPercentage =
		(slab_total - get_depot_unrecovered_slab_count(depot)) * 100 /
		slab_total;

	VDOState state = get_vdo_state(vdo);
	stats->inRecoveryMode = (state == VDO_RECOVERING);
	snprintf(stats->mode,
		 sizeof(stats->mode),
		 "%s",
		 describe_vdo_state(state));
}

/**********************************************************************/
BlockCount get_physical_blocks_allocated(const struct vdo *vdo)
{
	return (get_depot_allocated_blocks(vdo->depot) -
		get_journal_block_map_data_blocks_used(vdo->recovery_journal));
}

/**********************************************************************/
BlockCount get_physical_blocks_free(const struct vdo *vdo)
{
	return get_depot_free_blocks(vdo->depot);
}

/**********************************************************************/
BlockCount get_physical_blocks_overhead(const struct vdo *vdo)
{
	// XXX config.physicalBlocks is actually mutated during resize and is in
	// a packed structure, but resize runs on admin thread so we're usually
	// OK.
	return (vdo->config.physicalBlocks - get_depot_data_blocks(vdo->depot) +
		get_journal_block_map_data_blocks_used(vdo->recovery_journal));
}

/**********************************************************************/
BlockCount get_total_block_map_blocks(const struct vdo *vdo)
{
	return (get_number_of_fixed_block_map_pages(vdo->block_map) +
		get_journal_block_map_data_blocks_used(vdo->recovery_journal));
}

/**********************************************************************/
WritePolicy get_write_policy(const struct vdo *vdo)
{
	return vdo->load_config.writePolicy;
}

/**********************************************************************/
void set_write_policy(struct vdo *vdo, WritePolicy new)
{
	vdo->load_config.writePolicy = new;
}

/**********************************************************************/
const VDOLoadConfig *get_vdo_load_config(const struct vdo *vdo)
{
	return &vdo->load_config;
}

/**********************************************************************/
const struct thread_config *get_thread_config(const struct vdo *vdo)
{
	return vdo->load_config.threadConfig;
}

/**********************************************************************/
BlockCount get_configured_block_map_maximum_age(const struct vdo *vdo)
{
	return vdo->load_config.maximumAge;
}

/**********************************************************************/
PageCount get_configured_cache_size(const struct vdo *vdo)
{
	return vdo->load_config.cacheSize;
}

/**********************************************************************/
PhysicalBlockNumber get_first_block_offset(const struct vdo *vdo)
{
	return vdo->load_config.firstBlockOffset;
}

/**********************************************************************/
struct block_map *get_block_map(const struct vdo *vdo)
{
	return vdo->block_map;
}

/**********************************************************************/
struct slab_depot *get_slab_depot(struct vdo *vdo)
{
	return vdo->depot;
}

/**********************************************************************/
struct recovery_journal *get_recovery_journal(struct vdo *vdo)
{
	return vdo->recovery_journal;
}

/**********************************************************************/
void dump_vdo_status(const struct vdo *vdo)
{
	dump_flusher(vdo->flusher);
	dump_recovery_journal_statistics(vdo->recovery_journal);
	dump_packer(vdo->packer);
	dump_slab_depot(vdo->depot);

	const struct thread_config *thread_config = get_thread_config(vdo);
	ZoneCount zone;
	for (zone = 0; zone < thread_config->logicalZoneCount; zone++) {
		dump_logical_zone(get_logical_zone(vdo->logical_zones, zone));
	}

	for (zone = 0; zone < thread_config->physicalZoneCount; zone++) {
		dump_physical_zone(vdo->physical_zones[zone]);
	}

	for (zone = 0; zone < thread_config->hashZoneCount; zone++) {
		dump_hash_zone(vdo->hash_zones[zone]);
	}
}

/**********************************************************************/
void set_vdo_tracing_flags(struct vdo *vdo, bool vioTracing)
{
	vdo->vio_trace_recording = vioTracing;
}

/**********************************************************************/
bool vdo_vio_tracing_enabled(const struct vdo *vdo)
{
	return ((vdo != NULL) && vdo->vio_trace_recording);
}

/**********************************************************************/
void assert_on_admin_thread(struct vdo *vdo, const char *name)
{
	ASSERT_LOG_ONLY((getCallbackThreadID() ==
			 getAdminThread(get_thread_config(vdo))),
			"%s called on admin thread",
			name);
}

/**********************************************************************/
void assert_on_logical_zone_thread(const struct vdo *vdo,
				   ZoneCount logical_zone,
				   const char *name)
{
	ASSERT_LOG_ONLY((getCallbackThreadID() ==
			 getLogicalZoneThread(get_thread_config(vdo),
					      logical_zone)),
			"%s called on logical thread",
			name);
}

/**********************************************************************/
void assert_on_physical_zone_thread(const struct vdo *vdo,
				    ZoneCount physical_zone,
				    const char *name)
{
	ASSERT_LOG_ONLY((getCallbackThreadID() ==
			 getPhysicalZoneThread(get_thread_config(vdo),
					       physical_zone)),
			"%s called on physical thread",
			name);
}

/**********************************************************************/
struct hash_zone *select_hash_zone(const struct vdo *vdo,
				   const UdsChunkName *name)
{
	/*
	 * Use a fragment of the chunk name as a hash code. To ensure uniform
	 * distributions, it must not overlap with fragments used elsewhere.
	 * Eight bits of hash should suffice since the number of hash zones is
	 * small.
	 */
	// XXX Make a central repository for these offsets ala hashUtils.
	// XXX Verify that the first byte is independent enough.
	uint32_t hash = name->name[0];

	/*
	 * Scale the 8-bit hash fragment to a zone index by treating it as a
	 * binary fraction and multiplying that by the zone count. If the hash
	 * is uniformly distributed over [0 .. 2^8-1], then (hash * count / 2^8)
	 * should be uniformly distributed over [0 .. count-1]. The multiply and
	 * shift is much faster than a divide (modulus) on X86 CPUs.
	 */
	return vdo->hash_zones[(hash * get_thread_config(vdo)->hashZoneCount) >>
			       8];
}

/**********************************************************************/
int get_physical_zone(const struct vdo *vdo,
		      PhysicalBlockNumber pbn,
		      struct physical_zone **zone_ptr)
{
	if (pbn == ZERO_BLOCK) {
		*zone_ptr = NULL;
		return VDO_SUCCESS;
	}

	// Used because it does a more restrictive bounds check than get_slab(),
	// and done first because it won't trigger read-only mode on an invalid
	// PBN.
	if (!is_physical_data_block(vdo->depot, pbn)) {
		return VDO_OUT_OF_RANGE;
	}

	// With the PBN already checked, we should always succeed in finding a
	// slab.
	struct vdo_slab *slab = get_slab(vdo->depot, pbn);
	int result =
		ASSERT(slab != NULL, "get_slab must succeed on all valid PBNs");
	if (result != VDO_SUCCESS) {
		return result;
	}

	*zone_ptr = vdo->physical_zones[get_slab_zone_number(slab)];
	return VDO_SUCCESS;
}

/**********************************************************************/
struct zoned_pbn validate_dedupe_advice(struct vdo *vdo,
					const struct data_location *advice,
					LogicalBlockNumber lbn)
{
	struct zoned_pbn no_advice = { .pbn = ZERO_BLOCK };
	if (advice == NULL) {
		return no_advice;
	}

	// Don't use advice that's clearly meaningless.
	if ((advice->state == MAPPING_STATE_UNMAPPED) ||
	    (advice->pbn == ZERO_BLOCK)) {
		logDebug("Invalid advice from deduplication server: pbn %" PRIu64
 ", state %u. Giving up on deduplication of logical block %llu",
			 advice->pbn,
			 advice->state,
			 lbn);
		atomicAdd64(&vdo->error_stats.invalidAdvicePBNCount, 1);
		return no_advice;
	}

	struct physical_zone *zone;
	int result = get_physical_zone(vdo, advice->pbn, &zone);
	if ((result != VDO_SUCCESS) || (zone == NULL)) {
		logDebug("Invalid physical block number from deduplication server: %llu, giving up on deduplication of logical block %llu",
			 advice->pbn,
			 lbn);
		atomicAdd64(&vdo->error_stats.invalidAdvicePBNCount, 1);
		return no_advice;
	}

	return (struct zoned_pbn) {
		.pbn = advice->pbn,
		.state = advice->state,
		.zone = zone,
	};
}
