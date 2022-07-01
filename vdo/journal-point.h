/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef JOURNAL_POINT_H
#define JOURNAL_POINT_H

#include "numeric.h"
#include "types.h"

typedef uint16_t journal_entry_count_t;

/*
 * The absolute position of an entry in a recovery journal or slab journal.
 */
struct journal_point {
	sequence_number_t sequence_number;
	journal_entry_count_t entry_count;
};

/*
 * A packed, platform-independent encoding of a struct journal_point.
 */
struct packed_journal_point {
	/*
	 * The packed representation is the little-endian 64-bit representation
	 * of the low-order 48 bits of the sequence number, shifted up 16 bits,
	 * or'ed with the 16-bit entry count.
	 *
	 * Very long-term, the top 16 bits of the sequence number may not always
	 * be zero, as this encoding assumes--see BZ 1523240.
	 */
	__le64 encoded_point;
} __packed;

/**
 * vdo_advance_journal_point() - Move the given journal point forward by one
 *                               entry.
 * @point: The journal point to adjust.
 * @entries_per_block: The number of entries in one full block.
 */
static inline void
vdo_advance_journal_point(struct journal_point *point,
			  journal_entry_count_t entries_per_block)
{
	point->entry_count++;
	if (point->entry_count == entries_per_block) {
		point->sequence_number++;
		point->entry_count = 0;
	}
}

/**
 * vdo_is_valid_journal_point() - Check whether a journal point is valid.
 * @point: The journal point.
 *
 * Return: true if the journal point is valid.
 */
static inline bool
vdo_is_valid_journal_point(const struct journal_point *point)
{
	return ((point != NULL) && (point->sequence_number > 0));
}

/**
 * vdo_before_journal_point() - Check whether the first point precedes the
 *                              second point.
 * @first: The first journal point.
 * @second: The second journal point.
 *
 * Return: true if the first point precedes the second point.
 */
static inline bool vdo_before_journal_point(const struct journal_point *first,
					    const struct journal_point *second)
{
	return ((first->sequence_number < second->sequence_number) ||
		((first->sequence_number == second->sequence_number) &&
		 (first->entry_count < second->entry_count)));
}

/**
 * vdo_are_equivalent_journal_points() - Check whether the first point is the
 *                                       same as the second point.
 * @first: The first journal point.
 * @second: The second journal point.
 *
 * Return: true if both points reference the same logical position of an entry
 *         in the journal.
 */
static inline bool
vdo_are_equivalent_journal_points(const struct journal_point *first,
				  const struct journal_point *second)
{
	return ((first->sequence_number == second->sequence_number) &&
		(first->entry_count == second->entry_count));
}

/**
 * vdo_pack_journal_point() - Encode the journal location represented by a
 *                            journal_point into a packed_journal_point.
 * @unpacked: The unpacked input point.
 * @packed: The packed output point.
 */
static inline void vdo_pack_journal_point(const struct journal_point *unpacked,
					  struct packed_journal_point *packed)
{
	packed->encoded_point = __cpu_to_le64((unpacked->sequence_number << 16)
					      | unpacked->entry_count);
}

/**
 * vdo_unpack_journal_point() - Decode the journal location represented by a
 *                              packed_journal_point into a journal_point.
 * @packed: The packed input point.
 * @unpacked: The unpacked output point.
 */
static inline void
vdo_unpack_journal_point(const struct packed_journal_point *packed,
			 struct journal_point *unpacked)
{
	uint64_t native = __le64_to_cpu(packed->encoded_point);

	unpacked->sequence_number = (native >> 16);
	unpacked->entry_count = (native & 0xffff);
}

#endif /* JOURNAL_POINT_H */
