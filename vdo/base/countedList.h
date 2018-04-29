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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/base/countedList.h#1 $
 */

#ifndef COUNTED_LIST_H
#define COUNTED_LIST_H

#include "ringNode.h"
#include "types.h"

/**
 * A counted list is a doubly-linked list which keeps track of its length.
 * The list elements are RingNodes.
 **/
typedef struct {
  RingNode head;
  size_t   length;
} CountedList;

/**
 * Initialize an empty CountedList.
 *
 * @param list  The list to initialize
 **/
static inline void initializeCountedList(CountedList *list)
{
  initializeRing(&list->head);
  list->length = 0;
}

/**
 * Check whether a counted list is empty.
 *
 * @param list  The list to check
 *
 * @return <code>true</code> if the list is empty
 **/
static inline bool isCountedListEmpty(const CountedList *list)
{
  return (list->length == 0);
}

/**
 * Remove a node from a list. This method does not check that the node is
 * actually on the specified list.
 *
 * @param list  The list from which to remove the node
 * @param node  The node to remove
 *
 * @return the removed node, for convenience
 **/
static inline RingNode *removeCountedListNode(CountedList *list,
                                              RingNode    *node)
{
  list->length--;
  return unspliceRingNode(node);
}

/**
 * Add a node to the head of the list.
 *
 * @param list  The list to be added to
 * @param node  The node to add to the list
 **/
static inline void addToCountedListHead(CountedList *list, RingNode *node)
{
  list->length++;
  spliceRingChainAfter(node, node, &list->head);
}

/**
 * Add a node to the tail of the list.
 *
 * @param list  The list to be added to
 * @param node  The node to add to the list
 **/
static inline void addToCountedListTail(CountedList *list, RingNode *node)
{
  list->length++;
  spliceRingChainBefore(node, node, &list->head);
}

/**
 * Transfer the contents of a CountedList to the tail of another CountedList.
 * The donor list will become empty.
 *
 * @param from  The list to transfer from
 * @param to    The list to transfer to
 **/
static inline void transferToCountedListTail(CountedList *from,
                                             CountedList *to)
{
  if (isCountedListEmpty(from)) {
    return;
  }

  to->length   += from->length;
  from->length  = 0;
  spliceRingChainBefore(from->head.next, from->head.prev, &to->head);
}

/**
 * Remove a node from the head of the list.
 *
 * @param list  The list from which to remove a node
 *
 * @return NULL if the list was empty, otherwise the first node in the list
 **/
static inline RingNode *removeFromCountedListHead(CountedList *list)
{
  RingNode *node = chopRingNode(&list->head);
  if (node != NULL) {
    list->length--;
  }
  return node;
}

/**
 * Remove a node from the tail of the list.
 *
 * @param list  The list from which to remove a node
 *
 * @return NULL if the list was empty, otherwise the first node in the list
 **/
static inline RingNode *removeFromCountedListTail(CountedList *list)
{
  RingNode *node = popRingNode(&list->head);
  if (node != NULL) {
    list->length--;
  }
  return node;
}

#endif // COUNTED_LIST_H
