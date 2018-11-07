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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bioIterator.h#1 $
 */

#ifndef BIO_ITERATOR_H
#define BIO_ITERATOR_H

#include <linux/bio.h>

#include "bio.h"
#include "kernelTypes.h"

typedef struct {
  BIO              *bio;
#ifdef USE_BI_ITER
  struct bvec_iter  iter;
  // Needed so we can store the return value of bio_iter_iovec.
  struct bio_vec    temp;
#else
  int               index;
#endif
} BioIterator;

/**
 * Create an iterator over a bio's data.
 *
 * @param bio   The bio to iterate over
 *
 * @return An iterator over a bio
 **/
static BioIterator createBioIterator(BIO *bio)
{
  BioIterator iterator = {
    .bio   = bio,
#ifdef USE_BI_ITER
    .iter  = bio->bi_iter,
#else
    .index = bio->bi_idx,
#endif
  };
  return iterator;
}

/**
 * Get the next biovec from the iterator, or NULL if there are no more.
 *
 * @param iterator      The iterator from which to get data
 *
 * @return The next biovec from the iterator, or NULL.
 **/
static struct bio_vec *getNextBiovec(BioIterator *iterator)
{
  BIO *bio = iterator->bio;
#ifdef USE_BI_ITER
  if (iterator->iter.bi_size == 0) {
    return NULL;
  }

  iterator->temp = bio_iter_iovec(bio, iterator->iter);
  return &iterator->temp;
#else
  if (iterator->index >= bio->bi_vcnt) {
    return NULL;
  }
  return bio_iovec_idx(bio, iterator->index);
#endif
}

/**
 * Advance the iterator to the next biovec in the bio.
 *
 * @param [in,out] iterator     The iterator to advance
 **/
static void advanceBioIterator(BioIterator *iterator)
{
#ifdef USE_BI_ITER
  bio_advance_iter(iterator->bio, &iterator->iter, iterator->temp.bv_len);
#else
  iterator->index++;
#endif
}

#endif /* BIO_ITERATOR_H */
