/*
 * Copyright Red Hat
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
 */

#ifndef PRIORITY_TABLE_H
#define PRIORITY_TABLE_H

#include <linux/list.h>

/**
 * A priority_table is a simple implementation of a priority queue for entries
 * with priorities that are small non-negative integer values. It implements
 * the obvious priority queue operations of enqueuing an entry and dequeuing
 * an entry with the maximum priority. It also supports removing an arbitrary
 * entry. The priority of an entry already in the table can be changed by
 * removing it and re-enqueuing it with a different priority. All operations
 * have O(1) complexity.
 *
 * The links for the table entries must be embedded in the entries themselves.
 * Lists are used to link entries in the table and no wrapper type is
 * declared, so an existing list entry in an object can also be used to
 * queue it in a priority_table, assuming the field is not used for anything
 * else while so queued.
 *
 * The table is implemented as an array of queues (circular lists) indexed by
 * priority, along with a hint for which queues are non-empty. Steven Skiena
 * calls a very similar structure a "bounded height priority queue", but given
 * the resemblance to a hash table, "priority table" seems both shorter and
 * more apt, if somewhat novel.
 **/

struct priority_table;

/**
 * Allocate and initialize a new priority_table.
 *
 * @param [in]  max_priority  The maximum priority value for table entries
 * @param [out] table_ptr     A pointer to hold the new table
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check make_priority_table(unsigned int max_priority,
				     struct priority_table **table_ptr);

/**
 * Free a priority_table. NOTE: The table does not own the entries stored in
 * it and they are not freed by this call.
 *
 * @param table  The table to free
 **/
void free_priority_table(struct priority_table *table);

/**
 * Add a new entry to the priority table, appending it to the queue for
 * entries with the specified priority.
 *
 * @param table     The table in which to store the entry
 * @param priority  The priority of the entry
 * @param entry     The list_head embedded in the entry to store in the table
 *                  (the caller must have initialized it)
 **/
void priority_table_enqueue(struct priority_table *table, unsigned int priority,
			    struct list_head *entry);

/**
 * Reset a priority table, leaving it in the same empty state as when newly
 * constructed. NOTE: The table does not own the entries stored in it and they
 * are not freed (or even unlinked from each other) by this call.
 *
 * @param table  The table to reset
 **/
void reset_priority_table(struct priority_table *table);

/**
 * Find the highest-priority entry in the table, remove it from the table, and
 * return it. If there are multiple entries with the same priority, the one
 * that has been in the table with that priority the longest will be returned.
 *
 * @param table  The priority table from which to remove an entry
 *
 * @return the dequeued entry, or NULL if the table is currently empty
 **/
struct list_head * __must_check
priority_table_dequeue(struct priority_table *table);

/**
 * Remove a specified entry from its priority table.
 *
 * @param table   The table from which to remove the entry
 * @param entry   The entry to remove from the table
 **/
void priority_table_remove(struct priority_table *table,
			   struct list_head *entry);

/**
 * Return whether the priority table is empty.
 *
 * @param table   The table to check
 *
 * @return <code>true</code> if the table is empty
 **/
bool __must_check is_priority_table_empty(struct priority_table *table);

#endif /* PRIORITY_TABLE_H */
