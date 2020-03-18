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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/upgrade.c#16 $
 */

#include "upgrade.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "blockMap.h"
#include "readOnlyNotifier.h"
#include "recoveryJournal.h"
#include "releaseVersions.h"
#include "slabDepot.h"
#include "statusCodes.h"
#include "superBlock.h"
#include "vdoInternal.h"
#include "volumeGeometry.h"

/* The latest supported Sodium version */
/* Commented out because not currently used.
 * static const struct version_number SODIUM_MASTER_VERSION_67_0 = {
 * .major_version = 67,
 * .minor_version =  0,
 * };
 */

/* The component data version for current Sodium */
static const struct version_number SODIUM_COMPONENT_DATA_41_0 = {
	.major_version = 41,
	.minor_version = 0,
};

/**
 * Current Sodium's configuration of the VDO component.
 **/
struct sodium_component_41_0 {
	VDOState state;
	uint64_t complete_recoveries;
	uint64_t read_only_recoveries;
	VDOConfig config;
	Nonce nonce;
} __attribute__((packed));

/**
 * Checks whether the release version loaded in the superblock is the
 * current VDO version.
 *
 * @param vdo  The vdo to validate
 *
 * @return true if the release version number is the current version
 **/
static bool is_current_release_version(struct vdo *vdo)
{
	ReleaseVersionNumber loaded_version =
		get_loaded_release_version(vdo->superBlock);

	return (loaded_version == CURRENT_RELEASE_VERSION_NUMBER);
}

/**
 * Loads the VDO master version into the vdo and checks that the version
 * can be understood by vdo.
 *
 * @param vdo  The vdo to validate
 *
 * @return VDO_SUCCESS or an error if the loaded version is not supported
 **/
static int validate_sodium_version(struct vdo *vdo)
{
	int result = decodeVDOVersion(vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (is_current_release_version(vdo)) {
		return VDO_SUCCESS;
	}

	ReleaseVersionNumber loaded_version =
		get_loaded_release_version(vdo->superBlock);
	return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
				       "Release version %d, load version %d.%d"
				       " cannot be upgraded",
				       loaded_version,
				       vdo->loadVersion.major_version,
				       vdo->loadVersion.minor_version);
}

/**
 * Decode a sodium_component_41_0 structure.
 *
 * @param buffer        The component data buffer
 * @param component     The component structure to decode into
 *
 * @return VDO_SUCCESS or an error code
 **/
static int decode_sodium_41_0_component(Buffer *buffer,
					struct sodium_component_41_0 *component)
{
	return getBytesFromBuffer(buffer, sizeof(*component), component);
}

/**
 * Decode the component data for the VDO itself from the component data
 * buffer in the super block.
 *
 * @param vdo     The vdo to decode
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
decode_sodium_component(struct vdo *vdo)
{
	Buffer *buffer = get_component_buffer(vdo->superBlock);
	struct version_number version;
	int result = decode_version_number(buffer, &version);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct sodium_component_41_0 component;
	if (are_same_version(SODIUM_COMPONENT_DATA_41_0, version)) {
		result = decode_sodium_41_0_component(buffer, &component);
	} else {
		return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
					       "VDO component data version mismatch, expected 41.0, got %d.%d",
					       version.major_version,
					       version.minor_version);
	}
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Copy the decoded component into the vdo structure.
	setVDOState(vdo, component.state);
	vdo->loadState = component.state;
	vdo->completeRecoveries = component.complete_recoveries;
	vdo->readOnlyRecoveries = component.read_only_recoveries;
	vdo->config = component.config;
	vdo->nonce = component.nonce;

	logInfo("Converted VDO component data version %d.%d",
		version.major_version,
		version.minor_version);
	return VDO_SUCCESS;
}

/**********************************************************************/
__attribute__((warn_unused_result)) static int
finish_sodium_decode(struct vdo *vdo)
{
	Buffer *buffer = get_component_buffer(vdo->superBlock);
	const ThreadConfig *threadConfig = getThreadConfig(vdo);
	int result =
		make_recovery_journal(vdo->nonce,
				      vdo->layer,
				      get_vdo_partition(vdo->layout,
							RECOVERY_JOURNAL_PARTITION),
				      vdo->completeRecoveries,
				      vdo->config.recoveryJournalSize,
				      RECOVERY_JOURNAL_TAIL_BUFFER_SIZE,
				      vdo->readOnlyNotifier,
				      threadConfig,
				      &vdo->recoveryJournal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_sodium_recovery_journal(vdo->recoveryJournal, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_sodium_slab_depot(buffer,
					  threadConfig,
					  vdo->nonce,
					  vdo->layer,
					  get_vdo_partition(vdo->layout,
							    SLAB_SUMMARY_PARTITION),
					  vdo->readOnlyNotifier,
					  vdo->recoveryJournal,
					  &vdo->depot);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_sodium_block_map(buffer,
					 vdo->config.logicalBlocks,
					 threadConfig,
					 &vdo->blockMap);
	if (result != VDO_SUCCESS) {
		return result;
	}

	ASSERT_LOG_ONLY((contentLength(buffer) == 0),
			"All decoded component data was used");
	return VDO_SUCCESS;
}

/**********************************************************************/
int upgrade_prior_vdo(PhysicalLayer *layer)
{
	struct volume_geometry geometry;
	int result = load_volume_geometry(layer, &geometry);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct vdo *vdo;
	result = makeVDO(layer, &vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = load_super_block(vdo->layer,
				  get_data_region_offset(geometry),
				  &vdo->superBlock);
	if (result != VDO_SUCCESS) {
		freeVDO(&vdo);
		return logErrorWithStringError(result,
					       "Could not load VDO super block");
	}

	// Load the necessary pieces to save again.
	result = validate_sodium_version(vdo);
	if (result != VDO_SUCCESS) {
		freeVDO(&vdo);
		return result;
	}

	if (is_current_release_version(vdo)) {
		logInfo("VDO already up-to-date");
		freeVDO(&vdo);
		return VDO_SUCCESS;
	}

	result = decode_sodium_component(vdo);
	if (result != VDO_SUCCESS) {
		freeVDO(&vdo);
		return result;
	}

	if (requiresRebuild(vdo)) {
		// Do not attempt to upgrade a dirty prior version.
		freeVDO(&vdo);
		return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
					       "Cannot upgrade a dirty VDO.");
	}

	result = decode_vdo_layout(get_component_buffer(vdo->superBlock),
				   &vdo->layout);
	if (result != VDO_SUCCESS) {
		freeVDO(&vdo);
		return result;
	}

	const ThreadConfig *thread_config = getThreadConfig(vdo);
	result = make_read_only_notifier(inReadOnlyMode(vdo),
					 thread_config,
					 vdo->layer,
					 &vdo->readOnlyNotifier);
	if (result != VDO_SUCCESS) {
		freeVDO(&vdo);
		return result;
	}

	result = finish_sodium_decode(vdo);
	if (result != VDO_SUCCESS) {
		freeVDO(&vdo);
		return result;
	}

	// Saving will automatically change the release version to current.
	result = saveVDOComponents(vdo);
	if (result != VDO_SUCCESS) {
		freeVDO(&vdo);
		return result;
	}

	logInfo("Successfully saved upgraded VDO");
	freeVDO(&vdo);

	return result;
}
