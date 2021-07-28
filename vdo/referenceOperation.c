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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/referenceOperation.c#3 $
 */

#include "referenceOperation.h"

#include "physicalZone.h"
#include "types.h"

/**********************************************************************/
static struct pbn_lock *return_pbn_lock(struct reference_operation operation)
{
	return (struct pbn_lock *) operation.context;
}

/**********************************************************************/
void
set_up_vdo_reference_operation_with_lock(enum journal_operation type,
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

/**********************************************************************/
static struct pbn_lock *look_up_pbn_lock(struct reference_operation operation)
{
	return ((operation.context == NULL)
			? NULL
			: get_vdo_physical_zone_pbn_lock(operation.context,
							 operation.pbn));
}

/**********************************************************************/
void
set_up_vdo_reference_operation_with_zone(enum journal_operation type,
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
