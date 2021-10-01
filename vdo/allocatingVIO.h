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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/allocatingVIO.h#38 $
 */

#ifndef ALLOCATING_VIO_H
#define ALLOCATING_VIO_H

#include "permassert.h"

#include "pbnLock.h"
#include "physicalZone.h"
#include "types.h"
#include "vdo.h"
#include "vio.h"
#include "waitQueue.h"

typedef void allocation_callback(struct allocating_vio *allocation_vio);

/**
 * A vio which can receive an allocation from the block allocator. Currently,
 * these are used both for servicing external data requests and for compressed
 * block writes.
 **/
struct allocating_vio {
	/** The underlying vio */
	struct vio vio;

	/** The wait_queue entry structure */
	struct waiter waiter;

	/** The physical zone in which to allocate a physical block */
	struct physical_zone *zone;

	/** The block allocated to this vio */
	physical_block_number_t allocation;

	/**
	 * If non-NULL, the pooled PBN lock held on the allocated block. Must
	 * be a write lock until the block has been written, after which it
	 * will become a read lock.
	 **/
	struct pbn_lock *allocation_lock;

	/** The type of write lock to obtain on the allocated block */
	enum pbn_lock_type write_lock_type;

	/** The number of zones in which this vio has attempted to allocate */
	zone_count_t allocation_attempts;

	/** Whether this vio should wait for a clean slab */
	bool wait_for_clean_slab;

	/** The function to call once allocation is complete */
	allocation_callback *allocation_callback;
};

/**
 * Convert a vio to an allocating_vio.
 *
 * @param vio  The vio to convert
 *
 * @return The vio as an allocating_vio
 **/
static inline struct allocating_vio *vio_as_allocating_vio(struct vio *vio)
{
	ASSERT_LOG_ONLY(((vio->type == VIO_TYPE_DATA) ||
			 (vio->type == VIO_TYPE_COMPRESSED_BLOCK)),
			"vio is an allocating_vio");
	return container_of(vio, struct allocating_vio, vio);
}

/**
 * Convert an allocating_vio to a vio.
 *
 * @param allocating_vio  The allocating_vio to convert
 *
 * @return The allocating_vio as a vio
 **/
static inline struct vio *
allocating_vio_as_vio(struct allocating_vio *allocating_vio)
{
	return &allocating_vio->vio;
}

/**
 * Convert a generic vdo_completion to an allocating_vio.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as an allocating_vio
 **/
static inline struct allocating_vio *
as_allocating_vio(struct vdo_completion *completion)
{
	return vio_as_allocating_vio(as_vio(completion));
}

/**
 * Convert an allocating_vio to a generic completion.
 *
 * @param allocating_vio  The allocating_vio to convert
 *
 * @return The allocating_vio as a completion
 **/
static inline struct vdo_completion *
allocating_vio_as_completion(struct allocating_vio *allocating_vio)
{
	return vio_as_completion(allocating_vio_as_vio(allocating_vio));
}

/**
 * Convert an allocating_vio to a generic wait queue entry.
 *
 * @param allocating_vio  The allocating_vio to convert
 *
 * @return The allocating_vio as a wait queue entry
 **/
static inline struct waiter *
allocating_vio_as_waiter(struct allocating_vio *allocating_vio)
{
	return &allocating_vio->waiter;
}

/**
 * Convert an allocating_vio's generic wait queue entry back to the
 * allocating_vio.
 *
 * @param waiter  The wait queue entry to convert
 *
 * @return The wait queue entry as an allocating_vio
 **/
static inline struct allocating_vio *
waiter_as_allocating_vio(struct waiter *waiter)
{
	if (waiter == NULL) {
		return NULL;
	}

	return container_of(waiter, struct allocating_vio, waiter);
}

/**
 * Get the vdo from an allocating_vio.
 *
 * @param allocating_vio  The allocating_vio from which to get the vdo
 *
 * @return The vdo to which an allocating_vio belongs
 **/
static inline struct vdo *
get_vdo_from_allocating_vio(struct allocating_vio *allocating_vio)
{
	return get_vdo_from_vio(allocating_vio_as_vio(allocating_vio));
}

/**
 * Check that an allocating_vio is running on the physical zone thread in
 * which it did its allocation.
 *
 * @param allocating_vio  The allocating_vio in question
 **/
static inline void
assert_vio_in_physical_zone(struct allocating_vio *allocating_vio)
{
	thread_id_t expected =
		get_vdo_physical_zone_thread_id(allocating_vio->zone);
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((expected == thread_id),
			"struct allocating_vio for allocated physical block %llu on thread %u, should be on thread %u",
			(unsigned long long) allocating_vio->allocation,
			thread_id,
			expected);
}

/**
 * Set a callback as a physical block operation in an allocating_vio's
 * allocated zone.
 *
 * @param allocating_vio  The allocating_vio
 * @param callback        The callback to set
 **/
static inline void
vio_set_physical_zone_callback(struct allocating_vio *allocating_vio,
			       vdo_action *callback)
{
	set_vdo_completion_callback(allocating_vio_as_completion(allocating_vio),
				    callback,
				    get_vdo_physical_zone_thread_id(allocating_vio->zone));
}

/**
 * Set a callback as a physical block operation in an allocating_vio's
 * allocated zone and invoke it immediately.
 *
 * @param allocating_vio  The allocating_vio
 * @param callback       The callback to invoke
 **/
static inline void
vio_launch_physical_zone_callback(struct allocating_vio *allocating_vio,
				  vdo_action *callback)
{
	vio_set_physical_zone_callback(allocating_vio, callback);
	invoke_vdo_completion_callback(allocating_vio_as_completion(allocating_vio));
}

/**
 * Allocate a data block to an allocating_vio.
 *
 * @param allocating_vio   The allocating_vio which needs an allocation
 * @param selector         The allocation selector for deciding which physical
 *                         zone to allocate from
 * @param write_lock_type  The type of write lock to obtain on the block
 * @param callback         The function to call once the allocation is complete
 **/
void vio_allocate_data_block(struct allocating_vio *allocating_vio,
			     struct allocation_selector *selector,
			     enum pbn_lock_type write_lock_type,
			     allocation_callback *callback);

/**
 * Release the PBN lock on the allocated block. If the reference to the locked
 * block is still provisional, it will be released as well.
 *
 * @param allocating_vio  The lock holder
 **/
void vio_release_allocation_lock(struct allocating_vio *allocating_vio);

/**
 * Reset an allocating_vio after it has done an allocation.
 *
 * @param allocating_vio  The allocating_vio
 **/
void vio_reset_allocation(struct allocating_vio *allocating_vio);

/**
 * Create a new allocating_vio for compressed writes.
 *
 * @param [in]  vdo                 The vdo
 * @param [in]  parent              The parent to assign to the allocating_vio's
 *                                  completion
 * @param [in]  data                The buffer
 * @param [out] allocating_vio_ptr  A pointer to hold new allocating_vio
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
create_compressed_write_vio(struct vdo *vdo,
			    void *parent,
			    char *data,
			    struct allocating_vio **allocating_vio_ptr);

#endif // ALLOCATING_VIO_H
