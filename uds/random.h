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
 * $Id: //eng/uds-releases/gloria/src/uds/random.h#1 $
 */

#ifndef RANDOM_H
#define RANDOM_H

#include "common.h"
#include "compiler.h"
#include "randomDefs.h"
#include "typeDefs.h"

/**
 * Create a random block name.
 *
 * @param [out] name    the resulting random block name
 **/

static INLINE void createRandomBlockName(UdsChunkName *name)
{
  fillRandomly(name->name, UDS_CHUNK_NAME_SIZE);
}

/**
 * Create random block metadata.
 *
 * @param [out] data    the result random metadata
 **/
static INLINE void createRandomMetadata(UdsChunkData *data)
{
  fillRandomly(data->data, UDS_MAX_BLOCK_DATA_SIZE);
}

/**
 * Get random unsigned integer in a given range
 *
 * @param lo  Minimum unsigned integer value
 * @param hi  Maximum unsigned integer value
 *
 * @return unsigned integer in the interval [lo,hi]
 **/
unsigned int randomInRange(unsigned int lo, unsigned int hi);

/**
 * Special function wrapper required for compile-time assertions. This
 * function will fail to compile RAND_MAX is not of the form 2^n - 1.
 **/
void randomCompileTimeAssertions(void);

#endif /* RANDOM_H */
