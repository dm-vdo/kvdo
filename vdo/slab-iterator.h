/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SLAB_ITERATOR_H
#define SLAB_ITERATOR_H

#include "slab.h"
#include "types.h"

/*
 * A slab_iterator is a structure for iterating over a set of slabs.
 */
struct slab_iterator {
	struct vdo_slab **slabs;
	struct vdo_slab *next;
	slab_count_t end;
	slab_count_t stride;
};

/**
 * vdo_iterate_slabs() - Return a slab_iterator initialized to iterate
 *                       over an array of slabs with a given stride.
 * @slabs: The array of slabs.
 * @start: The number of the slab to start iterating from.
 * @end: The number of the last slab which may be returned.
 * @stride: The difference in slab number between successive slabs.
 *
 * Iteration always occurs from higher to lower numbered slabs.
 *
 * Return: An initialized iterator structure.
 */
static inline struct slab_iterator vdo_iterate_slabs(struct vdo_slab **slabs,
						     slab_count_t start,
						     slab_count_t end,
						     slab_count_t stride)
{
	return (struct slab_iterator) {
		.slabs = slabs,
		.next = (((slabs == NULL) || (start < end)) ? NULL
							    : slabs[start]),
		.end = end,
		.stride = stride,
	};
}

/**
 * vdo_has_next_slab() - Check whether another vdo_slab would be returned by
 *                       the iterator.
 * @iterator: The iterator to poll.
 *
 * Return: true if the next call to vdo_next_slab will return a vdo_slab.
 */
static inline bool vdo_has_next_slab(const struct slab_iterator *iterator)
{
	return (iterator->next != NULL);
}

/**
 * vdo_next_slab() - Get the next vdo_slab, advancing the iterator.
 * @iterator: The iterator over the vdo_slab chain.
 *
 * Return: The next vdo_slab or NULL if the array of slabs is empty or if all
 *         the appropriate Slabs have been returned.
 */
static inline struct vdo_slab *vdo_next_slab(struct slab_iterator *iterator)
{
	struct vdo_slab *slab = iterator->next;

	if ((slab == NULL)
	    || (slab->slab_number < iterator->end + iterator->stride)) {
		iterator->next = NULL;
	} else {
		iterator->next =
			iterator->slabs[slab->slab_number - iterator->stride];
	}
	return slab;
}

#endif /* SLAB_ITERATOR_H */
