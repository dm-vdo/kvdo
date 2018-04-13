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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabIterator.h#1 $
 */

#ifndef SLAB_ITERATOR_H
#define SLAB_ITERATOR_H

#include "slab.h"
#include "types.h"

/**
 * SlabIterator is a structure for iterating over a set of slabs.
 **/
typedef struct {
  Slab      **slabs;
  Slab       *next;
  SlabCount   end;
  SlabCount   stride;
} SlabIterator;

/**
 * Return a SlabIterator initialized to iterate over an array of slabs
 * with a given stride. Iteration always occurs from higher to lower numbered
 * slabs.
 *
 * @param slabs  The array of slabs
 * @param start  The number of the slab to start iterating from
 * @param end    The number of the last slab which may be returned
 * @param stride The difference in slab number between successive slabs
 *
 * @return an initialized iterator structure
 **/
static inline SlabIterator iterateSlabs(Slab      **slabs,
                                        SlabCount   start,
                                        SlabCount   end,
                                        SlabCount   stride)
{
  return (SlabIterator) {
    .slabs  = slabs,
    .next   = (((slabs == NULL) || (start < end)) ? NULL : slabs[start]),
    .end    = end,
    .stride = stride,
  };
}

/**
 * Check whether another Slab would be returned by the iterator.
 *
 * @param iterator  The iterator to poll
 *
 * @return <code>true</code> if the next call to <code>nextSlab</code>
 *         will return a Slab
 **/
static inline bool hasNextSlab(const SlabIterator *iterator)
{
  return (iterator->next != NULL);
}

/**
 * Get the next Slab, advancing the iterator.
 *
 * @param iterator  The iterator over the Slab chain
 *
 * @return the next Slab or <code>NULL</code> if the array of slabs is empty
 *         or if all the appropriate Slabs have been returned
 **/
static inline Slab *nextSlab(SlabIterator *iterator)
{
  Slab *slab = iterator->next;
  if ((slab == NULL)
      || (slab->slabNumber < iterator->end + iterator->stride)) {
    iterator->next = NULL;
  } else {
    iterator->next = iterator->slabs[slab->slabNumber - iterator->stride];
  }
  return slab;
}

#endif // SLAB_ITERATOR_H
