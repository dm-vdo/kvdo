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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/ringNode.h#1 $
 */

#ifndef RING_NODE_H
#define RING_NODE_H

#include "types.h"

/**
 * A ring node is a member of a doubly-linked circular list.
 *
 * Each node is usually embedded within a data structure that contains the
 * relevant payload. In addition the ring head is also represented by a
 * node where the next field designates the first element of the ring and the
 * prev field designates the last.
 *
 * An empty ring contains next and prev fields that point back to the ring
 * head itself.
 *
 * Typical iteration over a ring, from the front and back:
 *
 * for (RingNode *n = head->next; n != head; n = n->next) { ... }
 * for (RingNode *p = head->prev; p != head; p = p->prev) { ... }
 **/
typedef struct ringNode RingNode;

struct ringNode {
  RingNode *next;
  RingNode *prev;
};

/**
 * Initialize a ring to be empty.
 *
 * @param head The head of the ring
 **/
static inline void initializeRing(RingNode *head)
{
  head->next = head->prev = head;
}

/**
 * Check whether a ring is empty.
 *
 * @param head The head of the ring
 *
 * @return <code>true</code> if the ring is empty
 **/
static inline bool isRingEmpty(const RingNode *head)
{
  return (head->next == head);
}

/**
 * Check whether a ring contains exactly one node.
 *
 * @param head  The head of the ring
 *
 * @return <code>true</code> if the ring contains exactly one member
 **/
static inline bool isRingSingleton(const RingNode *head)
{
  return (!isRingEmpty(head) && (head->prev == head->next));
}

/**
 * Unsplice a contiguous chain of at least one node from its ring.
 *
 * @param first         the first entry in the ring to unsplice
 * @param last          the last entry in the ring to unsplice,
 *                      may be the same as ``first``
 *
 * The effect of this is to create two rings, the one designated
 * by first through last, and the other consisting of anything remaining.
 **/
static inline void unspliceRingChain(RingNode *first,
                                     RingNode *last)
{
  first->prev->next = last->next;
  last->next->prev = first->prev;
  first->prev = last;
  last->next = first;
}

/**
 * Remove a ring node from its ring.
 *
 * @param node  the ring node
 *
 * @return the removed node, for convenience
 **/
static inline RingNode *unspliceRingNode(RingNode *node)
{
  unspliceRingChain(node, node);
  return node;
}

/**
 * Splice a contiguous chain of at least one node after the specified entry,
 * which may be the head of a ring.
 *
 * @param first         the first entry in a contiguous span of nodes
 * @param last          the last entry in a contiguous span of nodes,
 *                      may be the same as ``first``
 * @param where         the entry after which ``first`` through ``last``
 *                      shall appear
 *
 * The effect of this is to unsplice first through last (if necessary) and
 * insert them after ``where`` so that the previous nodes after ``where``
 * now appear after ``last``.
 **/
static inline void spliceRingChainAfter(RingNode *first,
                                        RingNode *last,
                                        RingNode *where)
{
  if (last->next != first) {
    unspliceRingChain(first, last);
  }
  last->next = where->next;
  first->prev = where;
  where->next->prev = last;
  where->next = first;
}

/**
 * Splice a contiguous chain of at least one node before the specified entry,
 * which may be the tail of a list.
 *
 * @param first         the first entry in a contiguous span of nodes
 * @param last          the last entry in a contiguous span of nodes,
 *                      may be the same as ``first``
 * @param where         the entry before which ``first`` through ``last``
 *                      shall appear
 *
 * The effect of this is to unsplice first through last (if necessary) and
 * insert them before ``where`` so that the previous nodes before ``where``
 * now appear before ``first``.
 **/
static inline void spliceRingChainBefore(RingNode *first,
                                         RingNode *last,
                                         RingNode *where)
{
  if (last->next != first) {
    unspliceRingChain(first, last);
  }
  first->prev = where->prev;
  last->next = where;
  where->prev->next = first;
  where->prev = last;
}

/**
 * Push a single node on the end of a ring.
 *
 * @param head The ring head
 * @param node The node to push
 **/
static inline void pushRingNode(RingNode *head, RingNode *node)
{
  spliceRingChainBefore(node, node, head);
}

/**
 * Pop a single node off the end of a ring.
 *
 * @param head  The ring head
 *
 * @return NULL if the ring was empty, otherwise the node that was
 *         removed from the ring (``head->prev``)
 **/
static inline RingNode *popRingNode(RingNode *head)
{
  return (isRingEmpty(head) ? NULL : unspliceRingNode(head->prev));
}

/**
 * Remove a single node off the front of the list
 **/
static inline RingNode *chopRingNode(RingNode *head)
{
  return (isRingEmpty(head) ? NULL : unspliceRingNode(head->next));
}

#endif // RING_NODE_H
