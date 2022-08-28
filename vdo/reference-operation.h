/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef REFERENCE_OPERATION_H
#define REFERENCE_OPERATION_H

#include "kernel-types.h"
#include "types.h"

struct reference_operation;

/**
 * typedef pbn_lock_getter - Get the pbn_lock associated with a
 *                           reference_operation.
 * @operation: The reference_operation.
 *
 * Return: The pbn_lock on the block of a reference_operation or NULL if there
 *         isn't one.
 */
typedef struct pbn_lock *pbn_lock_getter(struct reference_operation operation);

/*
 * The current operation on a physical block (from the point of view of the
 * data_vio doing the operation)
 */
struct reference_operation {
	/* The operation being performed */
	enum journal_operation type;
	/* The PBN of the block being operated on */
	physical_block_number_t pbn;
	/* The mapping state of the block being operated on */
	enum block_mapping_state state;
	/*
	 * A function to use to get any pbn_lock associated with this operation
	 */
	pbn_lock_getter *lock_getter;
	/* The context to pass to the pbn_lock_getter */
	void *context;
};

/**
 * vdo_get_reference_operation_pbn_lock() - Get the pbn_lock associated with
 *                                          the current reference_operation.
 * @operation: The reference operation.
 *
 * Return: The pbn_lock on the block of the current operation or NULL if there
 *         isn't one.
 */
static inline struct pbn_lock * __must_check
vdo_get_reference_operation_pbn_lock(struct reference_operation operation)
{
	return ((operation.lock_getter == NULL)
			? NULL
			: operation.lock_getter(operation));
}

void
vdo_set_up_reference_operation_with_lock(enum journal_operation type,
					 physical_block_number_t pbn,
					 enum block_mapping_state state,
					 struct pbn_lock *lock,
					 struct reference_operation *operation);

void
vdo_set_up_reference_operation_with_zone(enum journal_operation type,
					 physical_block_number_t pbn,
					 enum block_mapping_state state,
					 struct physical_zone *zone,
					 struct reference_operation *operation);

#endif /* REFERENCE_OPERATION_H */
