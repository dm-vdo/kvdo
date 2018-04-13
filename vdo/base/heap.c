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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/heap.c#1 $
 */

#include "heap.h"

#include "errors.h"
#include "logger.h"
#include "numeric.h"

#include "statusCodes.h"

/**********************************************************************/
void initializeHeap(Heap           *heap,
                    HeapComparator *comparator,
                    void           *array,
                    size_t          capacity,
                    size_t          elementSize)
{
  *heap = (Heap) {
    .comparator  = comparator,
    .capacity    = capacity,
    .elementSize = elementSize,
  };
  if (array != NULL) {
    // Calculating child indexes is simplified by pretending the element array
    // is 1-based.
    heap->array = ((byte *) array - elementSize);
  }
}

/**********************************************************************/
static inline void swapElements(Heap *heap, size_t node1, size_t node2)
{
  byte temp[heap->elementSize];

  memcpy(temp,                &heap->array[node1], heap->elementSize);
  memcpy(&heap->array[node1], &heap->array[node2], heap->elementSize);
  memcpy(&heap->array[node2], temp,                heap->elementSize);
}

/**********************************************************************/
static void siftHeapDown(Heap *heap, size_t topNode, size_t lastNode)
{
  // Keep sifting until the sub-heap rooted at topNode has no children.
  size_t leftChild;
  while ((leftChild = (2 * topNode)) <= lastNode) {
    // If there are two children, select the largest child to swap with.
    size_t swapNode = leftChild;
    if (leftChild < lastNode) {
      size_t rightChild = leftChild + heap->elementSize;
      if (heap->comparator(&heap->array[leftChild],
                           &heap->array[rightChild]) < 0) {
        swapNode = rightChild;
      }
    }

    // Stop sifting if topNode is at least as large as its largest child,
    // which means the heap invariant was restored by the previous swap.
    if (heap->comparator(&heap->array[topNode], &heap->array[swapNode]) >= 0) {
      return;
    }

    // Swap the element we've been sifting down with the larger child.
    swapElements(heap, topNode, swapNode);

    // Descend into the sub-heap rooted at that child, going around the loop
    // again in place of a tail-recursive call to siftHeapDown().
    topNode = swapNode;
  }

  // We sifted the element all the way to a leaf node of the heap, so the heap
  // invariant has now been restored.
}

/**********************************************************************/
void buildHeap(Heap *heap, size_t count)
{
  heap->count = minSizeT(count, heap->capacity);

  if ((heap->count < 2) || (heap->elementSize == 0)) {
    return;
  }

  /*
   * All the leaf nodes are trivially valid sub-heaps. Starting with the parent
   * of the right-most leaf node, restore the heap invariant in that sub-heap
   * by sifting the top node of the sub-heap down into one of its children's
   * valid sub-heaps (or not, if the top node is already larger than its
   * children). Continue iterating through all the interior nodes in the heap,
   * in sort of a reverse breadth-first traversal, restoring the heap
   * invariant for each (increasingly larger) sub-heap until we reach the root
   * of the heap. Once we sift the root node down into one of its two valid
   * children, the entire heap must be valid, by induction.
   *
   * Even though we operate on every node and potentially perform an O(log N)
   * traversal for each node, the combined probabilities of actually needing
   * to do a swap and the heights of the sub-heaps sum to a constant, so
   * restoring a heap from the bottom-up like this has only O(N) complexity.
   */
  size_t size       = heap->elementSize;
  size_t lastParent = size * (heap->count / 2);
  size_t lastNode   = size * heap->count;
  for (size_t topNode = lastParent; topNode > 0; topNode -= size) {
    siftHeapDown(heap, topNode, lastNode);
  }
}

/**********************************************************************/
bool popMaxHeapElement(Heap *heap, void *elementPtr)
{
  if (heap->count == 0) {
    return false;
  }

  size_t rootNode = (heap->elementSize * 1);
  size_t lastNode = (heap->elementSize * heap->count);

  // Return the maximum element (the root of the heap) if the caller wanted it.
  if (elementPtr != NULL) {
    memcpy(elementPtr, &heap->array[rootNode], heap->elementSize);
  }

  // Move the right-most leaf node to the vacated root node, reducing the
  // number of elements by one and violating the heap invariant.
  if (rootNode != lastNode) {
    memcpy(&heap->array[rootNode], &heap->array[lastNode], heap->elementSize);
  }
  heap->count -= 1;
  lastNode    -= heap->elementSize;

  // Restore the heap invariant by sifting the root back down into the heap.
  siftHeapDown(heap, rootNode, lastNode);
  return true;
}

/**********************************************************************/
static inline size_t siftAndSort(Heap *heap, size_t rootNode, size_t lastNode)
{
  /*
   * We have a valid heap, so the largest unsorted element is now at the top
   * of the heap. That element belongs at the start of the partially-sorted
   * array, preceding all the larger elements that we've already removed
   * from the heap. Swap that largest unsorted element with the the
   * right-most leaf node in the heap, moving it to its sorted position in
   * the array.
   */
  swapElements(heap, rootNode, lastNode);
  // The sorted list is now one element larger and valid. The heap is
  // one element smaller, and invalid.
  lastNode -= heap->elementSize;
  // Restore the heap invariant by sifting the swapped element back down
  // into the heap.
  siftHeapDown(heap, rootNode, lastNode);
  return lastNode;
}

/**********************************************************************/
size_t sortHeap(Heap *heap)
{
  // All zero-length records are identical and therefore already sorted, as
  // are empty or singleton arrays.
  if ((heap->count < 2) || (heap->elementSize == 0)) {
    return heap->count;
  }

  // Get the byte array offset of the root node, and the right-most leaf node
  // in the 1-based array of records that will form the heap.
  size_t rootNode = (heap->elementSize * 1);
  size_t lastNode = (heap->elementSize * heap->count);

  while (lastNode > rootNode) {
    lastNode = siftAndSort(heap, rootNode, lastNode);
  }

  size_t count = heap->count;
  heap->count = 0;
  return count;
}

/**********************************************************************/
void *sortNextHeapElement(Heap *heap)
{
  if ((heap->count == 0) || (heap->elementSize == 0)) {
    return NULL;
  }

  // Get the byte array offset of the root node, and the right-most leaf node
  // in the 1-based array of records that will form the heap.
  size_t rootNode = (heap->elementSize * 1);
  size_t lastNode = (heap->elementSize * heap->count);
  if (heap->count > 1) {
    siftAndSort(heap, rootNode, lastNode);
  }
  heap->count--;

  return &heap->array[lastNode];
}
