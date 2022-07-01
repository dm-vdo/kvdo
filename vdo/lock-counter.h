/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef LOCK_COUNTER_H
#define LOCK_COUNTER_H

#include "completion.h"
#include "types.h"

/**
 * DOC: Lock Counters.
 *
 * A lock_counter provides a set of shared reference count locks which is safe
 * across multiple zones with a minimum of cross-thread synchronization
 * operations. For each lock in the set, it maintains a set of per-zone lock
 * counts, and a single, atomic count of the number of zones holding locks.
 * Whenever a zone's individual counter for a lock goes from 0 to 1, the
 * zone count for that lock is incremented. Whenever a zone's individual
 * counter for a lock goes from 1 to 0, the zone count for that lock is
 * decremented. If the zone count goes to 0, and the lock counter's
 * completion is not in use, the completion is launched to inform the counter's
 * owner that some lock has been released. It is the owner's responsibility to
 * check for which locks have been released, and to inform the lock counter
 * that it has received the notification by calling
 * vdo_acknowledge_lock_unlock().
 */

int __must_check vdo_make_lock_counter(struct vdo *vdo,
				       void *parent,
				       vdo_action callback,
				       thread_id_t thread_id,
				       zone_count_t logical_zones,
				       zone_count_t physical_zones,
				       block_count_t locks,
				       struct lock_counter **lock_counter_ptr);

void vdo_free_lock_counter(struct lock_counter *counter);

bool __must_check vdo_is_lock_locked(struct lock_counter *lock_counter,
				     block_count_t lock_number,
				     enum vdo_zone_type zone_type);

void vdo_initialize_lock_count(struct lock_counter *counter,
			       block_count_t lock_number,
			       uint16_t value);

void vdo_acquire_lock_count_reference(struct lock_counter *counter,
				      block_count_t lock_number,
				      enum vdo_zone_type zone_type,
				      zone_count_t zone_id);

void vdo_release_lock_count_reference(struct lock_counter *counter,
				      block_count_t lock_number,
				      enum vdo_zone_type zone_type,
				      zone_count_t zone_id);

void vdo_release_journal_zone_reference(struct lock_counter *counter,
					block_count_t lock_number);

void
vdo_release_journal_zone_reference_from_other_zone(struct lock_counter *counter,
						   block_count_t lock_number);

void vdo_acknowledge_lock_unlock(struct lock_counter *counter);

bool __must_check vdo_suspend_lock_counter(struct lock_counter *counter);

bool __must_check vdo_resume_lock_counter(struct lock_counter *counter);

#endif /* LOCK_COUNTER_H */
