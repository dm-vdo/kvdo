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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/journalPoint.h#2 $
 */

#ifndef JOURNAL_POINT_H
#define JOURNAL_POINT_H

#include "numeric.h"
#include "types.h"

typedef uint16_t JournalEntryCount;

/**
 * The absolute position of an entry in a recovery journal or slab journal.
 **/
struct journal_point {
  SequenceNumber    sequenceNumber;
  JournalEntryCount entryCount;
};

/**
 * A packed, platform-independent encoding of a struct journal_point.
 **/
struct packed_journal_point {
  /**
   * The packed representation is the little-endian 64-bit representation of
   * the low-order 48 bits of the sequence number, shifted up 16 bits, or'ed
   * with the 16-bit entry count.
   *
   * Very long-term, the top 16 bits of the sequence number may not always be
   * zero, as this encoding assumes--see BZ 1523240.
   **/
  byte encodedPoint[8];
} __attribute__((packed));

/**
 * Move the given journal point forward by one entry.
 *
 * @param point            the journal point to adjust
 * @param entriesPerBlock  the number of entries in one full block
 **/
static inline void advanceJournalPoint(struct journal_point  *point,
                                       JournalEntryCount      entriesPerBlock)
{
  point->entryCount++;
  if (point->entryCount == entriesPerBlock) {
    point->sequenceNumber++;
    point->entryCount = 0;
  }
}

/**
 * Check whether a journal point is valid.
 *
 * @param point  the journal point
 *
 * @return <code>true</code> if the journal point is valid
 **/
static inline bool isValidJournalPoint(const struct journal_point *point)
{
  return ((point != NULL) && (point->sequenceNumber > 0));
}

/**
 * Check whether the first point precedes the second point.
 *
 * @param first   the first journal point
 * @param second  the second journal point

 *
 * @return <code>true</code> if the first point precedes the second point.
 **/
static inline bool beforeJournalPoint(const struct journal_point *first,
                                      const struct journal_point *second)
{
  return ((first->sequenceNumber < second->sequenceNumber)
          || ((first->sequenceNumber == second->sequenceNumber)
              && (first->entryCount < second->entryCount)));
}

/**
 * Check whether the first point is the same as the second point.
 *
 * @param first   the first journal point
 * @param second  the second journal point
 *
 * @return <code>true</code> if both points reference the same logical
 *         position of an entry the journal
 **/
static inline bool areEquivalentJournalPoints(const struct journal_point *first,
                                              const struct journal_point *second)
{
  return ((first->sequenceNumber == second->sequenceNumber)
          && (first->entryCount  == second->entryCount));
}

/**
 * Encode the journal location represented by a journal_point into a
 * packed_journal_point.
 *
 * @param unpacked  The unpacked input point
 * @param packed    The packed output point
 **/
static inline void packJournalPoint(const struct journal_point  *unpacked,
                                    struct packed_journal_point *packed)
{
  uint64_t native = ((unpacked->sequenceNumber << 16) | unpacked->entryCount);
  storeUInt64LE(packed->encodedPoint, native);
}

/**
 * Decode the journal location represented by a packed_journal_point into a
 * journal_point.
 *
 * @param packed    The packed input point
 * @param unpacked  The unpacked output point
 **/
static inline void
unpackJournalPoint(const struct packed_journal_point  *packed,
                   struct journal_point               *unpacked)
{
  uint64_t native          = getUInt64LE(packed->encodedPoint);
  unpacked->sequenceNumber = (native >> 16);
  unpacked->entryCount     = (native & 0xffff);
}

#endif // JOURNAL_POINT_H
