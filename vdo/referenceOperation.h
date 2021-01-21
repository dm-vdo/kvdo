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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/referenceOperation.h#11 $
 */

#ifndef REFERENCE_OPERATION_H
#define REFERENCE_OPERATION_H

#include "types.h"

struct reference_operation;

/**
 * Get the pbn_lock associated with a reference_operation.
 *
 * @param operation  The reference_operation
 *
 * @return The pbn_lock on the block of a reference_operation or NULL if there
 *         isn't one
 **/
typedef struct pbn_lock *pbn_lock_getter(struct reference_operation operation);

/**
 * The current operation on a physical block (from the point of view of the
 * data_vio doing the operation)
 **/
struct reference_operation {
	/** The operation being performed */
	enum journal_operation type;
	/** The PBN of the block being operated on */
	physical_block_number_t pbn;
	/** The mapping state of the block being operated on */
	enum block_mapping_state state;
	/**
	 * A function to use to get any pbn_lock associated with this operation
	 */
	pbn_lock_getter *lock_getter;
	/** The context to pass to the pbn_lock_getter */
	void *context;
};

/**
 * Get the pbn_lock associated with the current reference_operation.
 *
 * @param operation  The reference operation
 *
 * @return The pbn_lock on the block of the current operation or NULL if there
 *         isn't one
 **/
static inline struct pbn_lock * __must_check
get_reference_operation_pbn_lock(struct reference_operation operation)
{
	return ((operation.lock_getter == NULL)
			? NULL
			: operation.lock_getter(operation));
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
void set_up_reference_operation_with_lock(enum journal_operation type,
					  physical_block_number_t pbn,
					  enum block_mapping_state state,
					  struct pbn_lock *lock,
					  struct reference_operation *operation);

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
void set_up_reference_operation_with_zone(enum journal_operation type,
					  physical_block_number_t pbn,
					  enum block_mapping_state state,
					  struct physical_zone *zone,
					  struct reference_operation *operation);

#endif // REFERENCE_OPERATION_H
