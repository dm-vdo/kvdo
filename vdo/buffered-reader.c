// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "buffered-reader.h"

#include "compiler.h"
#include "io-factory.h"
#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"


/*
 * The buffered reader allows efficient I/O for IO regions. The internal
 * buffer always reads aligned data from the underlying region.
 */
struct buffered_reader {
	/* IO factory owning the block device */
	struct io_factory *factory;
	/* The dm_bufio_client to read from */
	struct dm_bufio_client *client;
	/* The current dm_buffer */
	struct dm_buffer *buffer;
	/* The number of blocks that can be read from */
	sector_t limit;
	/* Number of the current block */
	sector_t block_number;
	/* Start of the buffer */
	byte *start;
	/* End of the data read from the buffer */
	byte *end;
};

static void read_ahead(struct buffered_reader *reader, sector_t block_number)
{
	if (block_number < reader->limit) {
		enum { MAX_READ_AHEAD = 4 };
		sector_t read_ahead = min((sector_t) MAX_READ_AHEAD,
					  reader->limit - block_number);

		dm_bufio_prefetch(reader->client, block_number, read_ahead);
	}
}

/*
 * Make a new buffered reader.
 *
 * @param factory      The IO factory creating the buffered reader
 * @param client       The dm_bufio_client to read from
 * @param block_limit  The number of blocks that may be read
 * @param reader_ptr   The pointer to hold the newly allocated buffered reader
 *
 * @return UDS_SUCCESS or error code
 */
int make_buffered_reader(struct io_factory *factory,
			 struct dm_bufio_client *client,
			 sector_t block_limit,
			 struct buffered_reader **reader_ptr)
{
	int result;
	struct buffered_reader *reader = NULL;

	result = UDS_ALLOCATE(1,
			      struct buffered_reader,
			      "buffered reader",
			      &reader);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*reader = (struct buffered_reader) {
		.factory = factory,
		.client = client,
		.buffer = NULL,
		.limit = block_limit,
		.block_number = 0,
		.start = NULL,
		.end = NULL,
	};

	read_ahead(reader, 0);
	get_uds_io_factory(factory);
	*reader_ptr = reader;
	return UDS_SUCCESS;
}

void free_buffered_reader(struct buffered_reader *reader)
{
	if (reader == NULL) {
		return;
	}

	if (reader->buffer != NULL) {
		dm_bufio_release(reader->buffer);
	}

	dm_bufio_client_destroy(reader->client);
	put_uds_io_factory(reader->factory);
	UDS_FREE(reader);
}

static int position_reader(struct buffered_reader *reader,
			   sector_t block_number,
			   off_t offset)
{
	if ((reader->end == NULL) || (block_number != reader->block_number)) {
		struct dm_buffer *buffer = NULL;
		void *data;

		if (block_number >= reader->limit) {
			return UDS_OUT_OF_RANGE;
		}

		if (reader->buffer != NULL) {
			dm_bufio_release(reader->buffer);
			reader->buffer = NULL;
		}

		data = dm_bufio_read(reader->client, block_number, &buffer);
		if (IS_ERR(data)) {
			return -PTR_ERR(data);
		}

		reader->buffer = buffer;
		reader->start = data;
		if (block_number == reader->block_number + 1) {
			read_ahead(reader, block_number + 1);
		}
	}

	reader->block_number = block_number;
	reader->end = reader->start + offset;
	return UDS_SUCCESS;
}

static size_t bytes_remaining_in_read_buffer(struct buffered_reader *reader)
{
	return (reader->end == NULL ?
		0 :
		reader->start + UDS_BLOCK_SIZE - reader->end);
}

static int reset_reader(struct buffered_reader *reader)
{
	sector_t block_number;

	if (bytes_remaining_in_read_buffer(reader) > 0) {
		return UDS_SUCCESS;
	}

	block_number = reader->block_number;
	if (reader->end != NULL) {
		++block_number;
	}

	return position_reader(reader, block_number, 0);
}

/*
 * Retrieve data from a buffered reader, reading from the region when needed.
 *
 * @param reader  The buffered reader
 * @param data    The buffer to read data into
 * @param length  The length of the data to read
 *
 * @return UDS_SUCCESS or an error code
 */
int read_from_buffered_reader(struct buffered_reader *reader,
			      void *data,
			      size_t length)
{
	byte *dp = data;
	int result = UDS_SUCCESS;
	size_t chunk;

	while (length > 0) {
		result = reset_reader(reader);
		if (result != UDS_SUCCESS) {
			break;
		}

		chunk = min(length, bytes_remaining_in_read_buffer(reader));
		memcpy(dp, reader->end, chunk);
		length -= chunk;
		dp += chunk;
		reader->end += chunk;
	}

	if (((result == UDS_OUT_OF_RANGE) || (result == UDS_END_OF_FILE)) &&
	    (dp - (byte *) data > 0)) {
		result = UDS_SHORT_READ;
	}

	return result;
}

/*
 * Verify that the data currently in the buffer matches the required value.
 *
 * @param reader  The buffered reader
 * @param value   The value that must match the buffer contents
 * @param length  The length of the value that must match
 *
 * @return UDS_SUCCESS or UDS_CORRUPT_DATA if the value does not match
 *
 * @note If the value matches, the matching contents are consumed. However,
 *       if the match fails, any buffer contents are left as is.
 */
int verify_buffered_data(struct buffered_reader *reader,
			 const void *value,
			 size_t length)
{
	int result = UDS_SUCCESS;
	size_t chunk;
	const byte *vp = value;
	sector_t start_block_number = reader->block_number;
	int start_offset = reader->end - reader->start;

	while (length > 0) {
		result = reset_reader(reader);
		if (result != UDS_SUCCESS) {
			result = UDS_CORRUPT_DATA;
			break;
		}

		chunk = min(length, bytes_remaining_in_read_buffer(reader));
		if (memcmp(vp, reader->end, chunk) != 0) {
			result = UDS_CORRUPT_DATA;
			break;
		}

		length -= chunk;
		vp += chunk;
		reader->end += chunk;
	}

	if (result != UDS_SUCCESS) {
		position_reader(reader, start_block_number, start_offset);
	}

	return result;
}
