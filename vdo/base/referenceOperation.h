/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/referenceOperation.h#1 $
 */

#ifndef REFERENCE_OPERATION_H
#define REFERENCE_OPERATION_H

#include "types.h"

typedef struct referenceOperation ReferenceOperation;

/**
 * Get the PBNLock associated with a ReferenceOperation.
 *
 * @param operation  The ReferenceOperation
 *
 * @return The PBNLock on the block of a ReferenceOperation or NULL if there
 *         isn't one
 **/
typedef PBNLock *PBNLockGetter(ReferenceOperation operation);

/**
 * The current operation on a physical block (from the point of view of the
 * DataVIO doing the operation)
 **/
struct referenceOperation {
  /** The operation being performed */
  JournalOperation     type;
  /** The PBN of the block being operated on */
  PhysicalBlockNumber  pbn;
  /** The mapping state of the block being operated on */
  BlockMappingState    state;
  /** A function to use to get any PBNLock associated with this operation */
  PBNLockGetter       *lockGetter;
  /** The context to pass to the PBNLockGetter */
  void                *context;
};

/**
 * Get the PBNLock associated with the current ReferenceOperation.
 *
 * @param operation  The reference operation
 *
 * @return The PBNLock on the block of the current operation or NULL if there
 *         isn't one
 **/
__attribute__((warn_unused_result))
static inline
PBNLock *getReferenceOperationPBNLock(ReferenceOperation operation)
{
  return ((operation.lockGetter == NULL)
          ? NULL : operation.lockGetter(operation));
}

/**
 * Set up a ReferenceOperation for which we already have the lock.
 *
 * @param type       The type of operation
 * @param pbn        The PBN of the block on which to operate
 * @param state      The mapping state of the block on which to operate
 * @param lock       The PBNLock to associate with the operation
 * @param operation  The ReferenceOperation to set up
 **/
void setUpReferenceOperationWithLock(JournalOperation     type,
                                     PhysicalBlockNumber  pbn,
                                     BlockMappingState    state,
                                     PBNLock             *lock,
                                     ReferenceOperation  *operation);

/**
 * Set up a ReferenceOperation for which we will need to look up the lock later.
 *
 * @param type       The type of operation
 * @param pbn        The PBN of the block on which to operate
 * @param state      The mapping state of the block on which to operate
 * @param zone       The PhysicalZone from which the PBNLock can be retrieved
 *                   when needed
 * @param operation  The ReferenceOperation to set up
 **/
void setUpReferenceOperationWithZone(JournalOperation     type,
                                     PhysicalBlockNumber  pbn,
                                     BlockMappingState    state,
                                     PhysicalZone        *zone,
                                     ReferenceOperation  *operation);

#endif // REFERENCE_OPERATION_H
