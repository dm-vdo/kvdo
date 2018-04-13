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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/priorityTable.h#1 $
 */

#ifndef PRIORITY_TABLE_H
#define PRIORITY_TABLE_H

#include "common.h"
#include "ringNode.h"

/**
 * A PriorityTable is a simple implementation of a priority queue for entries
 * with priorities that are small non-negative integer values. It implements
 * the obvious priority queue operations of enqueuing an entry and dequeuing
 * an entry with the maximum priority. It also supports removing an arbitrary
 * entry. The priority of an entry already in the table can be changed by
 * removing it and re-enqueuing it with a different priority. All operations
 * have O(1) complexity.
 *
 * The links for the table entries must be embedded in the entries themselves.
 * RingNode is used to link entries in the table and no wrapper type is
 * declared, so an existing RingNode link in an object can also be used to
 * queue it in a PriorityTable, assuming the field is not used for anything
 * else while so queued.
 *
 * The table is implemented as an array of queues (circular lists) indexed by
 * priority, along with a hint for which queues are non-empty. Steven Skiena
 * calls a very similar structure a "bounded height priority queue", but given
 * the resemblance to a hash table, "priority table" seems both shorter and
 * more apt, if somewhat novel.
 **/

typedef struct priorityTable PriorityTable;

/**
 * Allocate and initialize a new PriorityTable.
 *
 * @param [in]  maxPriority  The maximum priority value for table entries
 * @param [out] tablePtr     A pointer to hold the new table
 *
 * @return VDO_SUCCESS or an error code
 **/
int makePriorityTable(unsigned int maxPriority, PriorityTable **tablePtr)
  __attribute__((warn_unused_result));

/**
 * Free a PriorityTable and null out the reference to it. NOTE: The table does
 * not own the entries stored in it and they are not freed by this call.
 *
 * @param [in,out] tablePtr  The reference to the table to free
 **/
void freePriorityTable(PriorityTable **tablePtr);

/**
 * Add a new entry to the priority table, appending it to the queue for
 * entries with the specified priority.
 *
 * @param table     The table in which to store the entry
 * @param priority  The priority of the entry
 * @param entry     The RingNode embedded in the entry to store in the table
 *                  (the caller must have initialized it)
 **/
void priorityTableEnqueue(PriorityTable *table,
                          unsigned int   priority,
                          RingNode      *entry);

/**
 * Reset a priority table, leaving it in the same empty state as when newly
 * constructed. NOTE: The table does not own the entries stored in it and they
 * are not freed (or even unlinked from each other) by this call.
 *
 * @param table  The table to reset
 **/
void resetPriorityTable(PriorityTable *table);

/**
 * Find the highest-priority entry in the table, remove it from the table, and
 * return it. If there are multiple entries with the same priority, the one
 * that has been in the table with that priority the longest will be returned.
 *
 * @param table  The priority table from which to remove an entry
 *
 * @return the dequeued entry, or NULL if the table is currently empty
 **/
RingNode *priorityTableDequeue(PriorityTable *table)
  __attribute__((warn_unused_result));

/**
 * Remove a specified entry from its priority table.
 *
 * @param table   The table from which to remove the entry
 * @param entry   The entry to remove from the table
 **/
void priorityTableRemove(PriorityTable *table, RingNode *entry);

/**
 * Return whether the priority table is empty.
 *
 * @param table   The table to check
 *
 * @return <code>true</code> if the table is empty
 **/
bool isPriorityTableEmpty(PriorityTable *table)
  __attribute__((warn_unused_result));

#endif /* PRIORITY_TABLE_H */
