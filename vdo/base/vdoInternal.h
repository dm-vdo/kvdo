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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoInternal.h#36 $
 */

#ifndef VDO_INTERNAL_H
#define VDO_INTERNAL_H

#include "vdo.h"

#include "adminCompletion.h"
#include "adminState.h"
#include "atomic.h"
#include "header.h"
#include "packer.h"
#include "statistics.h"
#include "superBlock.h"
#include "readOnlyNotifier.h"
#include "types.h"
#include "uds.h"
#include "vdoLayout.h"
#include "vdoState.h"

/**
 * Error counters are atomic since updates can arrive concurrently from
 * arbitrary threads.
 **/
struct atomic_error_statistics {
	// Dedupe path error stats
	Atomic64 invalid_advice_pbn_count;
	Atomic64 no_space_error_count;
	Atomic64 read_only_error_count;
};

struct vdo {
	/* The state of this vdo */
	Atomic32 state;
	/* The read-only notifier */
	struct read_only_notifier *read_only_notifier;
	/* The number of times this vdo has recovered from a dirty state */
	uint64_t complete_recoveries;
	/* The number of times this vdo has recovered from a read-only state */
	uint64_t read_only_recoveries;
	/* The format-time configuration of this vdo */
	struct vdo_config config;
	/* The load-time configuration of this vdo */
	struct vdo_load_config load_config;
	/* The nonce for this vdo */
	nonce_t nonce;

	/* The super block */
	struct vdo_super_block *super_block;

	/* The physical storage below us */
	PhysicalLayer *layer;

	/* Our partitioning of the physical layer's storage */
	struct vdo_layout *layout;

	/* The block map */
	struct block_map *block_map;

	/* The journal for block map recovery */
	struct recovery_journal *recovery_journal;

	/* The slab depot */
	struct slab_depot *depot;

	/* The compressed-block packer */
	struct packer *packer;
	/* Whether incoming data should be compressed */
	AtomicBool compressing;

	/* The handler for flush requests */
	struct flusher *flusher;

	/* The master version of the vdo when loaded (for upgrading) */
	struct version_number load_version;
	/* The state the vdo was in when loaded (primarily for unit tests) */
	VDOState load_state;
	/* Whether VIO tracing is enabled */
	bool vio_trace_recording;

	/* The logical zones of this vdo */
	struct logical_zones *logical_zones;

	/* The physical zones of this vdo */
	struct physical_zone **physical_zones;

	/* The hash lock zones of this vdo */
	struct hash_zone **hash_zones;

	/* The completion for administrative operations */
	struct admin_completion admin_completion;

	/* The administrative state of the vdo */
	struct admin_state admin_state;

	/* Whether a close is required */
	bool close_required;

	/* Atomic global counts of error events */
	struct atomic_error_statistics error_stats;
};

/**
 * Get the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo  The vdo
 *
 * @return the current state of the vdo
 **/
VDOState get_vdo_state(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Set the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo    The vdo whose state is to be set
 * @param state  The new state of the vdo
 **/
void set_vdo_state(struct vdo *vdo, VDOState state);

/**
 * Get the component data size of a vdo.
 *
 * @param vdo  The vdo whose component data size is desired
 *
 * @return the component data size of the vdo
 **/
size_t get_component_data_size(struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Encode the vdo and save the super block synchronously.
 *
 * @param vdo  The vdo whose state is being saved
 *
 * @return VDO_SUCCESS or an error
 **/
int save_vdo_components(struct vdo *vdo) __attribute__((warn_unused_result));

/**
 * Encode the vdo and save the super block asynchronously. All non-user mode
 * super block savers should use this bottle neck instead of calling
 * saveSuperBlockAsync() directly.
 *
 * @param vdo     The vdo whose state is being saved
 * @param parent  The completion to notify when the save is complete
 **/
void save_vdo_components_async(struct vdo *vdo, struct vdo_completion *parent);

/**
 * Decode the vdo master version from the component data buffer in the super
 * block and store it in the vdo's loadVersion field.
 **/
int decode_vdo_version(struct vdo *vdo) __attribute__((warn_unused_result));

/**
 * Loads the vdo master version into the vdo and checks that the version
 * can be understood by vdo.
 *
 * @param vdo  The vdo to validate
 *
 * @return VDO_SUCCESS or an error if the loaded version is not supported
 **/
int validate_vdo_version(struct vdo *vdo) __attribute__((warn_unused_result));

/**
 * Decode the component data for the vdo itself from the component data buffer
 * in the super block.
 *
 * @param vdo  The vdo to decode
 *
 * @return VDO_SUCCESS or an error
 **/
int decode_vdo_component(struct vdo *vdo) __attribute__((warn_unused_result));

/**
 * Validate constraints on a VDO config.
 *
 * @param config           The VDO config
 * @param block_count      The block count of the VDO
 * @param require_logical  Set to <code>true</code> if the number logical blocks
 *                         must be configured (otherwise, it may be zero)
 *
 * @return a success or error code
 **/
int validate_vdo_config(const struct vdo_config *config,
			block_count_t block_count,
			bool require_logical)
	__attribute__((warn_unused_result));

/**
 * Enable a vdo to enter read-only mode on errors.
 *
 * @param vdo  The vdo to enable
 *
 * @return VDO_SUCCESS or an error
 **/
int enable_read_only_entry(struct vdo *vdo);

/**
 * Get the block map.
 *
 * @param vdo  The vdo whose block map is desired
 *
 * @return the block map from the vdo
 **/
struct block_map *get_block_map(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the slab depot from a vdo.
 *
 * @param vdo  The vdo whose slab depot is desired
 *
 * @return the slab depot from the vdo
 **/
struct slab_depot *get_slab_depot(struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Get the recovery journal from a vdo.
 *
 * @param vdo  The vdo whose recovery journal is desired
 *
 * @return the recovery journal from the vdo
 **/
struct recovery_journal *get_recovery_journal(struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Check whether a vdo is in read-only mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in read-only mode
 **/
bool in_read_only_mode(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Check whether the vdo requires a read-only mode rebuild.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo requires a read-only rebuild
 **/
bool requires_read_only_rebuild(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Check whether a vdo requires rebuilding.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo must be rebuilt
 **/
bool requires_rebuild(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Check whether a vdo should enter recovery mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo requires recovery
 **/
bool requires_recovery(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Check whether a vdo was replaying the recovery journal into the block map
 * when it crashed.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo crashed while reconstructing the
 *         block map
 **/
bool is_replaying(const struct vdo *vdo) __attribute__((warn_unused_result));

/**
 * Check whether the vdo is in recovery mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in recovery mode
 **/
bool in_recovery_mode(const struct vdo *vdo)
	__attribute__((warn_unused_result));

/**
 * Put the vdo into recovery mode
 *
 * @param vdo  The vdo
 **/
void enter_recovery_mode(struct vdo *vdo);

/**
 * Assert that we are running on the admin thread.
 *
 * @param vdo   The vdo
 * @param name  The name of the function which should be running on the admin
 *              thread (for logging).
 **/
void assert_on_admin_thread(struct vdo *vdo, const char *name);

/**
 * Assert that this function was called on the specified logical zone thread.
 *
 * @param vdo           The vdo
 * @param logical_zone  The number of the logical zone
 * @param name          The name of the calling function
 **/
void assert_on_logical_zone_thread(const struct vdo *vdo,
				   zone_count_t logical_zone,
				   const char *name);

/**
 * Assert that this function was called on the specified physical zone thread.
 *
 * @param vdo            The vdo
 * @param physical_zone  The number of the physical zone
 * @param name           The name of the calling function
 **/
void assert_on_physical_zone_thread(const struct vdo *vdo,
				    zone_count_t physical_zone,
				    const char *name);

/**
 * Select the hash zone responsible for locking a given chunk name.
 *
 * @param vdo   The vdo containing the hash zones
 * @param name  The chunk name
 *
 * @return  The hash zone responsible for the chunk name
 **/
struct hash_zone *select_hash_zone(const struct vdo *vdo,
				   const struct uds_chunk_name *name)
	__attribute__((warn_unused_result));

/**
 * Get the physical zone responsible for a given physical block number of a
 * data block in this vdo instance, or of the zero block (for which a NULL
 * zone is returned). For any other block number that is not in the range of
 * valid data block numbers in any slab, an error will be returned. This
 * function is safe to call on invalid block numbers; it will not put the vdo
 * into read-only mode.
 *
 * @param [in]  vdo       The vdo containing the physical zones
 * @param [in]  pbn       The PBN of the data block
 * @param [out] zone_ptr  A pointer to return the physical zone
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE if the block number is invalid
 *         or an error code for any other failure
 **/
int get_physical_zone(const struct vdo *vdo,
		      physical_block_number_t pbn,
		      struct physical_zone **zone_ptr)
	__attribute__((warn_unused_result));

/**********************************************************************/
// Asynchronous callback to share a duplicate block. This is only public so
// test code may compare it against the current callback in the completion.
void share_block(struct vdo_completion *completion);

#endif /* VDO_INTERNAL_H */
