// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "record-page.h"

#include "permassert.h"

static unsigned int
encode_tree(byte record_page[],
	    const struct uds_chunk_record *sorted_pointers[],
	    unsigned int next_record,
	    unsigned int node,
	    unsigned int node_count)
{
	if (node < node_count) {
		unsigned int child = (2 * node) + 1;

		next_record = encode_tree(record_page,
					  sorted_pointers,
					  next_record,
					  child,
					  node_count);

		/*
		 * In-order traversal: copy the contents of the next record
		 * into the page at the node offset.
		 */
		memcpy(&record_page[node * BYTES_PER_RECORD],
		       sorted_pointers[next_record],
		       BYTES_PER_RECORD);
		++next_record;

		next_record = encode_tree(record_page,
					  sorted_pointers,
					  next_record,
					  child + 1,
					  node_count);
	}
	return next_record;
}

int encode_record_page(const struct volume *volume,
		       const struct uds_chunk_record records[],
		       byte record_page[])
{
	int result;
	unsigned int records_per_page = volume->geometry->records_per_page;
	const struct uds_chunk_record **record_pointers =
		volume->record_pointers;

	/*
	 * Build an array of record pointers. We'll sort the pointers by the
	 * block names in the records, which is less work than sorting the
	 * record values.
	 */
	unsigned int i;

	for (i = 0; i < records_per_page; i++) {
		record_pointers[i] = &records[i];
	}

	STATIC_ASSERT(offsetof(struct uds_chunk_record, name) == 0);
	result = radix_sort(volume->radix_sorter,
				(const byte **) record_pointers,
				records_per_page,
				UDS_CHUNK_NAME_SIZE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * Use the sorted pointers to copy the records from the chapter to the
	 * record page in tree order.
	 */
	encode_tree(record_page, record_pointers, 0, 0, records_per_page);
	return UDS_SUCCESS;
}

bool search_record_page(const byte record_page[],
			const struct uds_chunk_name *name,
			const struct geometry *geometry,
			struct uds_chunk_data *metadata)
{
	/* The record page is just an array of chunk records. */
	const struct uds_chunk_record *records =
		(const struct uds_chunk_record *) record_page;

	/*
	 * The array of records is sorted by name and stored as a binary tree
	 * in heap order, so the root of the tree is the first array element.
	 */
	unsigned int node = 0;

	while (node < geometry->records_per_page) {
		const struct uds_chunk_record *record = &records[node];
		int result = memcmp(name, &record->name, UDS_CHUNK_NAME_SIZE);

		if (result == 0) {
			if (metadata != NULL) {
				*metadata = record->data;
			}
			return true;
		}
		/*
		 * The children of node N are in the heap at indexes 2N+1 and
		 * 2N+2.
		 */
		node = ((2 * node) + ((result < 0) ? 1 : 2));
	}
	return false;
}
