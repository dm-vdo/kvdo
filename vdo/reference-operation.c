// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "reference-operation.h"

#include "physical-zone.h"
#include "types.h"

static struct pbn_lock *return_pbn_lock(struct reference_operation operation)
{
	return (struct pbn_lock *) operation.context;
}

/**
 * vdo_set_up_reference_operation_with_lock() - Set up a reference_operation
 *                                              for which we already have the
 *                                              lock.
 * @type: The type of operation.
 * @pbn: The PBN of the block on which to operate.
 * @state: The mapping state of the block on which to operate.
 * @lock: The pbn_lock to associate with the operation.
 * @operation: The reference_operation to set up.
 */
void
vdo_set_up_reference_operation_with_lock(enum journal_operation type,
					 physical_block_number_t pbn,
					 enum block_mapping_state state,
					 struct pbn_lock *lock,
					 struct reference_operation *operation)
{
	*operation = (struct reference_operation) {
		.type = type,
		.pbn = pbn,
		.state = state,
		.lock_getter = return_pbn_lock,
		.context = lock,
	};
}

static struct pbn_lock *look_up_pbn_lock(struct reference_operation operation)
{
	return ((operation.context == NULL)
			? NULL
			: vdo_get_physical_zone_pbn_lock(operation.context,
							 operation.pbn));
}

/**
 * vdo_set_up_reference_operation_with_zone() - Set up a reference_operation
 *                                              for which we will need to look
 *                                              up the lock later.
 * @type: The type of operation.
 * @pbn: The PBN of the block on which to operate.
 * @state: The mapping state of the block on which to operate.
 * @zone: The physical_zone from which the pbn_lock can be retrieved when
 *        needed.
 * @operation: The reference_operation to set up.
 */
void
vdo_set_up_reference_operation_with_zone(enum journal_operation type,
					 physical_block_number_t pbn,
					 enum block_mapping_state state,
					 struct physical_zone *zone,
					 struct reference_operation *operation)
{
	*operation = (struct reference_operation) {
		.type = type,
		.pbn = pbn,
		.state = state,
		.lock_getter = look_up_pbn_lock,
		.context = zone,
	};
}
