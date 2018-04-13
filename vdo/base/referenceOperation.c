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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/referenceOperation.c#1 $
 */

#include "referenceOperation.h"

#include "physicalZone.h"
#include "types.h"

/**********************************************************************/
static PBNLock *returnPBNLock(ReferenceOperation operation)
{
  return (PBNLock *) operation.context;
}

/**********************************************************************/
void setUpReferenceOperationWithLock(JournalOperation     type,
                                     PhysicalBlockNumber  pbn,
                                     BlockMappingState    state,
                                     PBNLock             *lock,
                                     ReferenceOperation  *operation)
{
  *operation = (ReferenceOperation) {
    .type       = type,
    .pbn        = pbn,
    .state      = state,
    .lockGetter = returnPBNLock,
    .context    = lock,
  };
}

/**********************************************************************/
static PBNLock *lookUpPBNLock(ReferenceOperation operation)
{
  return ((operation.context == NULL)
          ? NULL : getPBNLock(operation.context, operation.pbn));
}

/**********************************************************************/
void setUpReferenceOperationWithZone(JournalOperation     type,
                                     PhysicalBlockNumber  pbn,
                                     BlockMappingState    state,
                                     PhysicalZone        *zone,
                                     ReferenceOperation  *operation)
{
  *operation = (ReferenceOperation) {
    .type       = type,
    .pbn        = pbn,
    .state      = state,
    .lockGetter = lookUpPBNLock,
    .context    = zone,
  };
}
