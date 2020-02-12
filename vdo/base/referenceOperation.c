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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/referenceOperation.c#5 $
 */

#include "referenceOperation.h"

#include "physicalZone.h"
#include "types.h"

/**********************************************************************/
static struct pbn_lock *returnPBNLock(struct reference_operation operation)
{
  return (struct pbn_lock *) operation.context;
}

/**********************************************************************/
void setUpReferenceOperationWithLock(JournalOperation            type,
                                     PhysicalBlockNumber         pbn,
                                     BlockMappingState           state,
                                     struct pbn_lock            *lock,
                                     struct reference_operation *operation)
{
  *operation = (struct reference_operation) {
    .type       = type,
    .pbn        = pbn,
    .state      = state,
    .lockGetter = returnPBNLock,
    .context    = lock,
  };
}

/**********************************************************************/
static struct pbn_lock *lookUpPBNLock(struct reference_operation operation)
{
  return ((operation.context == NULL)
          ? NULL : get_pbn_lock(operation.context, operation.pbn));
}

/**********************************************************************/
void setUpReferenceOperationWithZone(JournalOperation            type,
                                     PhysicalBlockNumber         pbn,
                                     BlockMappingState           state,
                                     struct physical_zone       *zone,
                                     struct reference_operation *operation)
{
  *operation = (struct reference_operation) {
    .type       = type,
    .pbn        = pbn,
    .state      = state,
    .lockGetter = lookUpPBNLock,
    .context    = zone,
  };
}
