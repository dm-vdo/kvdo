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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/allocatingVIO.h#18 $
 */

#ifndef ALLOCATING_VIO_H
#define ALLOCATING_VIO_H

#include "atomic.h"
#include "pbnLock.h"
#include "physicalZone.h"
#include "types.h"
#include "vio.h"
#include "waitQueue.h"

typedef void allocation_callback(struct allocating_vio *allocationVIO);

/**
 * A vio which can receive an allocation from the block allocator. Currently,
 * these are used both for servicing external data requests and for compressed
 * block writes.
 **/
struct allocating_vio {
	/** The underlying vio */
	struct vio vio;

	/** The WaitQueue entry structure */
	struct waiter waiter;

	/** The physical zone in which to allocate a physical block */
	struct physical_zone *zone;

	/** The block allocated to this vio */
	PhysicalBlockNumber allocation;

	/**
	 * If non-NULL, the pooled PBN lock held on the allocated block. Must be
	 * a write lock until the block has been  written, after which it will
	 * become a read lock.
	 **/
	struct pbn_lock *allocation_lock;

	/** The type of write lock to obtain on the allocated block */
	pbn_lock_type write_lock_type;

	/** The number of zones in which this vio has attempted an allocation */
	ZoneCount allocation_attempts;

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
 * Check whether an allocating_vio is a compressed block write.
 *
 * @param allocating_vio  The allocating_vio to check
 *
 * @return <code>true</code> if the allocating_vio is a compressed block write
 **/
static inline bool
is_compressed_write_allocating_vio(struct allocating_vio *allocating_vio)
{
	return is_compressed_write_vio(allocating_vio_as_vio(allocating_vio));
}

/**
 * Add a trace record for the current source location.
 *
 * @param allocating_vio  The allocating_vio structure to be updated
 * @param location       The source-location descriptor to be recorded
 **/
static inline void
allocating_vio_add_trace_record(struct allocating_vio *allocating_vio,
				const struct trace_location *location)
{
	vio_add_trace_record(allocating_vio_as_vio(allocating_vio), location);
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
	return allocating_vio_as_vio(allocating_vio)->vdo;
}

/**
 * Check that an allocating_vio is running on the physical zone thread in
 * which it did its allocation.
 *
 * @param allocating_vio  The allocating_vio in question
 **/
static inline void assertInPhysicalZone(struct allocating_vio *allocating_vio)
{
	ThreadID expected = get_physical_zone_thread_id(allocating_vio->zone);
	ThreadID threadID = getCallbackThreadID();
	ASSERT_LOG_ONLY((expected == threadID),
			"struct allocating_vio for allocated physical block %llu on thread %u, should be on thread %u",
			allocating_vio->allocation,
			threadID,
			expected);
}

/**
 * Set a callback as a physical block operation in an allocating_vio's allocated
 * zone.
 *
 * @param allocating_vio  The allocating_vio
 * @param callback        The callback to set
 * @param location        The tracing info for the call site
 **/
static inline void
set_physical_zone_callback(struct allocating_vio *allocating_vio,
			   vdo_action *callback,
			   const struct trace_location *location)
{
	set_callback(allocating_vio_as_completion(allocating_vio),
		     callback,
		     get_physical_zone_thread_id(allocating_vio->zone));
	allocating_vio_add_trace_record(allocating_vio, location);
}

/**
 * Set a callback as a physical block operation in an allocating_vio's allocated
 * zone and invoke it immediately.
 *
 * @param allocating_vio  The allocating_vio
 * @param callback       The callback to invoke
 * @param location       The tracing info for the call site
 **/
static inline void
launch_physical_zone_callback(struct allocating_vio *allocating_vio,
			      vdo_action *callback,
			      const struct trace_location *location)
{
	set_physical_zone_callback(allocating_vio, callback, location);
	invoke_callback(allocating_vio_as_completion(allocating_vio));
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
void allocate_data_block(struct allocating_vio *allocating_vio,
			 struct allocation_selector *selector,
			 pbn_lock_type write_lock_type,
			 allocation_callback *callback);

/**
 * Release the PBN lock on the allocated block. If the reference to the locked
 * block is still provisional, it will be released as well.
 *
 * @param allocating_vio  The lock holder
 **/
void release_allocation_lock(struct allocating_vio *allocating_vio);

/**
 * Reset an allocating_vio after it has done an allocation.
 *
 * @param allocating_vio  The allocating_vio
 **/
void reset_allocation(struct allocating_vio *allocating_vio);

#endif // ALLOCATING_VIO_H
