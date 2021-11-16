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

#include "reference-operation.h"

#include "physical-zone.h"
#include "types.h"

static struct pbn_lock *return_pbn_lock(struct reference_operation operation)
{
	return (struct pbn_lock *) operation.context;
}

/**
 * Set up a reference_operation for which we already have the lock.
 *
 * @param type       The type of operation
 * @param pbn        The PBN of the block on which to operate
 * @param state      The mapping state of the block on which to operate
 * @param lock       The pbn_lock to associate with the operation
 * @param operation  The reference_operation to set up
 **/
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
 * Set up a reference_operation for which we will need to look up the lock
 *later.
 *
 * @param type       The type of operation
 * @param pbn        The PBN of the block on which to operate
 * @param state      The mapping state of the block on which to operate
 * @param zone       The physical_zone from which the pbn_lock can be retrieved
 *                   when needed
 * @param operation  The reference_operation to set up
 **/
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
