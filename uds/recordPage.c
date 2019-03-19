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
 * $Id: //eng/uds-releases/gloria/src/uds/recordPage.c#1 $
 */

#include "recordPage.h"

#include "permassert.h"

/**********************************************************************/
static unsigned int encodeTree(byte                  recordPage[],
                               const UdsChunkRecord *sortedPointers[],
                               unsigned int          nextRecord,
                               unsigned int          node,
                               unsigned int          nodeCount)
{
  if (node < nodeCount) {
    unsigned int child = (2 * node) + 1;
    nextRecord = encodeTree(recordPage, sortedPointers, nextRecord,
                            child, nodeCount);

    // In-order traversal: copy the contents of the next record
    // into the page at the node offset.
    memcpy(&recordPage[node * BYTES_PER_RECORD],
           sortedPointers[nextRecord],
           BYTES_PER_RECORD);
    ++nextRecord;

    nextRecord = encodeTree(recordPage, sortedPointers, nextRecord,
                            child + 1, nodeCount);
  }
  return nextRecord;
}

/**********************************************************************/
int encodeRecordPage(const Volume *volume, const UdsChunkRecord records[])
{
  unsigned int recordsPerPage = volume->geometry->recordsPerPage;
  const UdsChunkRecord **recordPointers = volume->recordPointers;

  // Build an array of record pointers. We'll sort the pointers by the block
  // names in the records, which is less work than sorting the record values.
  for (unsigned int i = 0; i < recordsPerPage; i++) {
    recordPointers[i] = &records[i];
  }

  STATIC_ASSERT(offsetof(UdsChunkRecord, name) == 0);
  int result = radixSort(volume->radixSorter, (const byte **) recordPointers,
                         recordsPerPage, UDS_CHUNK_NAME_SIZE);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Use the sorted pointers to copy the records from the chapter to the
  // record page in tree order.
  encodeTree(volume->scratchPage, recordPointers, 0, 0, recordsPerPage);
  return UDS_SUCCESS;
}

/**********************************************************************/
bool searchRecordPage(const byte          recordPage[],
                      const UdsChunkName *name,
                      const Geometry     *geometry,
                      UdsChunkData       *metadata)
{
  // The record page is just an array of chunk records.
  const UdsChunkRecord *records = (const UdsChunkRecord *) recordPage;

  // The array of records is sorted by name and stored as a binary tree in
  // heap order, so the root of the tree is the first array element.
  unsigned int node = 0;
  while (node < geometry->recordsPerPage) {
    const UdsChunkRecord *record = &records[node];
    int result = memcmp(name, &record->name, UDS_CHUNK_NAME_SIZE);
    if (result == 0) {
      if (metadata != NULL) {
        *metadata = record->data;
      }
      return true;
    }
    // The children of node N are in the heap at indexes 2N+1 and 2N+2.
    node = ((2 * node) + ((result < 0) ? 1 : 2));
  }
  return false;
}
