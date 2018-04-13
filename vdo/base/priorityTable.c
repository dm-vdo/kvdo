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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/priorityTable.c#1 $
 */

#include "priorityTable.h"

#include "errors.h"
#include "memoryAlloc.h"
#include "numUtils.h"

#include "statusCodes.h"

/** We use a single 64-bit search vector, so the maximum priority is 63 */
enum { MAX_PRIORITY = 63 };

/**
 * All the entries with the same priority are queued in a circular list in a
 * bucket for that priority. The table is essentially an array of buckets.
 **/
typedef struct bucket {
  /** The head of a queue of table entries, all having the same priority */
  RingNode     queue;
  /** The priority of all the entries in this bucket */
  unsigned int priority;
} Bucket;

/**
 * A priority table is an array of buckets, indexed by priority. New entries
 * are added to the end of the queue in the appropriate bucket. The dequeue
 * operation finds the highest-priority non-empty bucket by searching a bit
 * vector represented as a single 8-byte word, which is very fast with
 * compiler and CPU support.
 **/
struct priorityTable {
  /** The maximum priority of entries that may be stored in this table */
  unsigned int maxPriority;
  /** A bit vector flagging all buckets that are currently non-empty */
  uint64_t     searchVector;
  /** The array of all buckets, indexed by priority */
  Bucket       buckets[];
};

/**
 * Convert a queue head to to the bucket that contains it.
 *
 * @param head  The bucket queue ring head pointer to convert
 *
 * @return the enclosing bucket
 **/
static inline Bucket *asBucket(RingNode *head)
{
  STATIC_ASSERT(offsetof(Bucket, queue) == 0);
  return (Bucket *) head;
}

/**********************************************************************/
int makePriorityTable(unsigned int maxPriority, PriorityTable **tablePtr)
{
  if (maxPriority > MAX_PRIORITY) {
    return UDS_INVALID_ARGUMENT;
  }

  PriorityTable *table;
  int result = ALLOCATE_EXTENDED(PriorityTable, maxPriority + 1, Bucket,
                                 __func__, &table);
  if (result != VDO_SUCCESS) {
    return result;
  }

  for (unsigned int priority = 0; priority <= maxPriority; priority++) {
    Bucket *bucket   = &table->buckets[priority];
    bucket->priority = priority;
    initializeRing(&bucket->queue);
  }

  table->maxPriority  = maxPriority;
  table->searchVector = 0;

  *tablePtr = table;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freePriorityTable(PriorityTable **tablePtr)
{
  PriorityTable *table = *tablePtr;
  if (table == NULL) {
    return;
  }

  // Unlink the buckets from any entries still in the table so the entries
  // won't be left with dangling pointers to freed memory.
  resetPriorityTable(table);

  FREE(table);
  *tablePtr = NULL;
}

/**********************************************************************/
void resetPriorityTable(PriorityTable *table)
{
  table->searchVector = 0;
  for (unsigned int priority = 0; priority <= table->maxPriority; priority++) {
    unspliceRingNode(&table->buckets[priority].queue);
  }
}

/**********************************************************************/
void priorityTableEnqueue(PriorityTable *table,
                          unsigned int   priority,
                          RingNode      *entry)
{
  ASSERT_LOG_ONLY((priority <= table->maxPriority),
                  "entry priority must be valid for the table");

  // Append the entry to the queue in the specified bucket.
  pushRingNode(&table->buckets[priority].queue, entry);

  // Flag the bucket in the search vector since it must be non-empty.
  table->searchVector |= (1ULL << priority);
}

/**********************************************************************/
static inline void markBucketEmpty(PriorityTable *table, Bucket *bucket)
{
  table->searchVector &= ~(1ULL << bucket->priority);
}

/**********************************************************************/
RingNode *priorityTableDequeue(PriorityTable *table)
{
  // Find the highest priority non-empty bucket by finding the highest-order
  // non-zero bit in the search vector.
  int topPriority = logBaseTwo(table->searchVector);

  if (topPriority < 0) {
    // All buckets are empty.
    return NULL;
  }

  // Dequeue the first entry in the bucket.
  Bucket   *bucket = &table->buckets[topPriority];
  RingNode *entry  = unspliceRingNode(bucket->queue.next);

  // Clear the bit in the search vector if the bucket has been emptied.
  if (isRingEmpty(&bucket->queue)) {
    markBucketEmpty(table, bucket);
  }

  return entry;
}

/**********************************************************************/
void priorityTableRemove(PriorityTable *table, RingNode *entry)
{
  // We can't guard against calls where the entry is on a ring for a different
  // table, but it's easy to deal with an entry not in any table or ring.
  if (isRingEmpty(entry)) {
    return;
  }

  // Remove the entry from the bucket ring, remembering a pointer to another
  // entry in the ring.
  RingNode *nextNode = entry->next;
  unspliceRingNode(entry);

  // If the rest of the ring is now empty, the next node must be the ring head
  // in the bucket and we can use it to update the search vector.
  if (isRingEmpty(nextNode)) {
    markBucketEmpty(table, asBucket(nextNode));
  }
}

/**********************************************************************/
bool isPriorityTableEmpty(PriorityTable *table)
{
  return (table->searchVector == 0);
}
