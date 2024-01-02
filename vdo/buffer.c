// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "buffer.h"

#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"
#include "type-defs.h"

/*
 * This is an implementation of a rolling buffer for marshalling data to and
 * from storage. The put methods add data to the end of the buffer and advance
 * the end pointer past the new data. The get methods return data from the
 * start of the buffer and advance the start pointer past anything
 * returned. Data is not actually removed until the buffer is cleared or
 * compacted, so the same data can be read multiple times if desired.
 */

/*
 * Create a buffer which wraps an existing byte array.
 *
 * @param bytes           The bytes to wrap
 * @param length          The length of the buffer
 * @param content_length  The length of the current contents of the buffer
 * @param buffer_ptr      A pointer to hold the buffer
 *
 * @return UDS_SUCCESS or an error code
 */
int wrap_buffer(byte *bytes,
		size_t length,
		size_t content_length,
		struct buffer **buffer_ptr)
{
	int result;
	struct buffer *buffer;

	result = ASSERT((content_length <= length),
			"content length, %zu, fits in buffer size, %zu",
			length,
			content_length);

	result = UDS_ALLOCATE(1, struct buffer, "buffer", &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	buffer->data = bytes;
	buffer->start = 0;
	buffer->end = content_length;
	buffer->length = length;
	buffer->wrapped = true;

	*buffer_ptr = buffer;
	return UDS_SUCCESS;
}

/*
 * Create a new buffer and allocate its memory.
 *
 * @param length      The length of the buffer
 * @param buffer_ptr  A pointer to hold the buffer
 *
 * @return UDS_SUCCESS or an error code
 */
int make_buffer(size_t size, struct buffer **new_buffer)
{
	int result;
	byte *data;
	struct buffer *buffer;

	result = UDS_ALLOCATE(size, byte, "buffer data", &data);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = wrap_buffer(data, size, 0, &buffer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(UDS_FORGET(data));
		return result;
	}

	buffer->wrapped = false;
	*new_buffer = buffer;
	return UDS_SUCCESS;
}

void free_buffer(struct buffer *buffer)
{
	if (buffer == NULL) {
		return;
	}

	if (!buffer->wrapped) {
		UDS_FREE(UDS_FORGET(buffer->data));
	}

	UDS_FREE(buffer);
}

size_t buffer_length(struct buffer *buffer)
{
	return buffer->length;
}

/* Return the amount of data currently in the buffer. */
size_t content_length(struct buffer *buffer)
{
	return buffer->end - buffer->start;
}

/* Return the amount of data that has already been processed. */
size_t uncompacted_amount(struct buffer *buffer)
{
	return buffer->start;
}

/* Return the amount of space available in the buffer. */
size_t available_space(struct buffer *buffer)
{
	return buffer->length - buffer->end;
}

/* Return the amount of the buffer that is currently utilized. */
size_t buffer_used(struct buffer *buffer)
{
	return buffer->end;
}

/*
 * Ensure that a buffer has a given amount of space available, compacting the
 * buffer if necessary. Returns true if the space is available.
 */
bool ensure_available_space(struct buffer *buffer, size_t bytes)
{
	if (available_space(buffer) >= bytes) {
		return true;
	}

	compact_buffer(buffer);
	return (available_space(buffer) >= bytes);
}

void clear_buffer(struct buffer *buffer)
{
	buffer->start = 0;
	buffer->end = buffer->length;
}

/*
 * Eliminate buffer contents which have been extracted. This function copies
 * any data between the start and end pointers to the beginning of the buffer,
 * moves the start pointer to the beginning, and the end pointer to the end of
 * the copied data.
 */
void compact_buffer(struct buffer *buffer)
{
	size_t bytes_to_move;

	if ((buffer->start == 0) || (buffer->end == 0)) {
		return;
	}

	bytes_to_move = buffer->end - buffer->start;
	memmove(buffer->data, buffer->data + buffer->start, bytes_to_move);
	buffer->start = 0;
	buffer->end = bytes_to_move;
}

/* Reset the end of buffer to a different position. */
int reset_buffer_end(struct buffer *buffer, size_t end)
{
	if (end > buffer->length) {
		return UDS_BUFFER_ERROR;
	}

	buffer->end = end;
	if (buffer->start > buffer->end) {
		buffer->start = buffer->end;
	}

	return UDS_SUCCESS;
}

/* Advance the start pointer by the specified number of bytes. */
int skip_forward(struct buffer *buffer, size_t bytes_to_skip)
{
	if (content_length(buffer) < bytes_to_skip) {
		return UDS_BUFFER_ERROR;
	}

	buffer->start += bytes_to_skip;
	return UDS_SUCCESS;
}

/* Rewind the start pointer by the specified number of bytes. */
int rewind_buffer(struct buffer *buffer, size_t bytes_to_rewind)
{
	if (buffer->start < bytes_to_rewind) {
		return UDS_BUFFER_ERROR;
	}

	buffer->start -= bytes_to_rewind;
	return UDS_SUCCESS;
}

/*
 * Check whether the start of the contents of a buffer matches a specified
 * array of bytes.
 */
bool has_same_bytes(struct buffer *buffer, const byte *data, size_t length)
{
	return ((content_length(buffer) >= length) &&
		(memcmp(buffer->data + buffer->start, data, length) == 0));
}

/* Check whether two buffers have the same contents. */
bool equal_buffers(struct buffer *buffer1, struct buffer *buffer2)
{
	return has_same_bytes(buffer1,
			      buffer2->data + buffer2->start,
			      content_length(buffer2));
}

int get_byte(struct buffer *buffer, byte *byte_ptr)
{
	if (content_length(buffer) < sizeof(byte)) {
		return UDS_BUFFER_ERROR;
	}

	*byte_ptr = buffer->data[buffer->start++];
	return UDS_SUCCESS;
}

int put_byte(struct buffer *buffer, byte b)
{
	if (!ensure_available_space(buffer, sizeof(byte))) {
		return UDS_BUFFER_ERROR;
	}

	buffer->data[buffer->end++] = b;
	return UDS_SUCCESS;
}

int get_bytes_from_buffer(struct buffer *buffer, size_t length,
			  void *destination)
{
	if (content_length(buffer) < length) {
		return UDS_BUFFER_ERROR;
	}

	memcpy(destination, buffer->data + buffer->start, length);
	buffer->start += length;
	return UDS_SUCCESS;
}

/*
 * Get a pointer to the current contents of the buffer. This will be a pointer
 * to the actual memory managed by the buffer. It is the caller's
 * responsibility to ensure that the buffer is not modified while this pointer
 * is in use.
 */
byte *get_buffer_contents(struct buffer *buffer)
{
	return buffer->data + buffer->start;
}

/*
 * Copy bytes out of a buffer as per get_bytes_from_buffer(). Memory will be
 * allocated to hold the copy.
 */
int copy_bytes(struct buffer *buffer, size_t length, byte **destination_ptr)
{
	int result;
	byte *destination;

	result = UDS_ALLOCATE(length,
			      byte,
			      "copy_bytes() buffer",
			      &destination);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_bytes_from_buffer(buffer, length, destination);
	if (result != UDS_SUCCESS) {
		UDS_FREE(destination);
	} else {
		*destination_ptr = destination;
	}

	return result;
}

int put_bytes(struct buffer *buffer, size_t length, const void *source)
{
	if (!ensure_available_space(buffer, length)) {
		return UDS_BUFFER_ERROR;
	}

	memcpy(buffer->data + buffer->end, source, length);
	buffer->end += length;
	return UDS_SUCCESS;
}

/*
 * Copy the contents of a source buffer into the target buffer. This is
 * equivalent to calling get_byte() on the source and put_byte() on the
 * target repeatedly.
 */
int put_buffer(struct buffer *target, struct buffer *source, size_t length)
{
	int result;

	if (content_length(source) < length) {
		return UDS_BUFFER_ERROR;
	}

	result = put_bytes(target, length, get_buffer_contents(source));
	if (result != UDS_SUCCESS) {
		return result;
	}

	source->start += length;
	return UDS_SUCCESS;
}

/* Put the specified number of zero bytes in the buffer. */
int zero_bytes(struct buffer *buffer, size_t length)
{
	if (!ensure_available_space(buffer, length)) {
		return UDS_BUFFER_ERROR;
	}

	memset(buffer->data + buffer->end, 0, length);
	buffer->end += length;
	return UDS_SUCCESS;
}

int get_boolean(struct buffer *buffer, bool *b)
{
	int result;
	byte value;

	result = get_byte(buffer, &value);
	if (result == UDS_SUCCESS) {
		*b = (value == 1);
	}

	return result;
}

int put_boolean(struct buffer *buffer, bool b)
{
	return put_byte(buffer, (byte) (b ? 1 : 0));
}

int get_uint16_le_from_buffer(struct buffer *buffer, uint16_t *ui)
{
	if (content_length(buffer) < sizeof(uint16_t)) {
		return UDS_BUFFER_ERROR;
	}

	decode_uint16_le(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

int put_uint16_le_into_buffer(struct buffer *buffer, uint16_t ui)
{
	if (!ensure_available_space(buffer, sizeof(uint16_t))) {
		return UDS_BUFFER_ERROR;
	}

	encode_uint16_le(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

int get_uint16_les_from_buffer(struct buffer *buffer, size_t count,
			       uint16_t *ui)
{
	unsigned int i;

	if (content_length(buffer) < (sizeof(uint16_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	for (i = 0; i < count; i++) {
		decode_uint16_le(buffer->data, &buffer->start, ui + i);
	}

	return UDS_SUCCESS;
}

int put_uint16_les_into_buffer(struct buffer *buffer,
			       size_t count,
			       const uint16_t *ui)
{
	unsigned int i;

	if (!ensure_available_space(buffer, sizeof(uint16_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	for (i = 0; i < count; i++) {
		encode_uint16_le(buffer->data, &buffer->end, ui[i]);
	}

	return UDS_SUCCESS;
}

int get_int32_le_from_buffer(struct buffer *buffer, int32_t *i)
{
	if (content_length(buffer) < sizeof(int32_t)) {
		return UDS_BUFFER_ERROR;
	}

	decode_int32_le(buffer->data, &buffer->start, i);
	return UDS_SUCCESS;
}

int get_uint32_le_from_buffer(struct buffer *buffer, uint32_t *ui)
{
	if (content_length(buffer) < sizeof(uint32_t)) {
		return UDS_BUFFER_ERROR;
	}

	decode_uint32_le(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

int put_uint32_le_into_buffer(struct buffer *buffer, uint32_t ui)
{
	if (!ensure_available_space(buffer, sizeof(uint32_t))) {
		return UDS_BUFFER_ERROR;
	}

	encode_uint32_le(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

int put_int64_le_into_buffer(struct buffer *buffer, int64_t i)
{
	if (!ensure_available_space(buffer, sizeof(int64_t))) {
		return UDS_BUFFER_ERROR;
	}

	encode_int64_le(buffer->data, &buffer->end, i);
	return UDS_SUCCESS;
}

int get_uint64_le_from_buffer(struct buffer *buffer, uint64_t *ui)
{
	if (content_length(buffer) < sizeof(uint64_t)) {
		return UDS_BUFFER_ERROR;
	}

	decode_uint64_le(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

int put_uint64_le_into_buffer(struct buffer *buffer, uint64_t ui)
{
	if (!ensure_available_space(buffer, sizeof(uint64_t))) {
		return UDS_BUFFER_ERROR;
	}

	encode_uint64_le(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

int get_uint64_les_from_buffer(struct buffer *buffer, size_t count,
			       uint64_t *ui)
{
	unsigned int i;

	if (content_length(buffer) < (sizeof(uint64_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	for (i = 0; i < count; i++) {
		decode_uint64_le(buffer->data, &buffer->start, ui + i);
	}

	return UDS_SUCCESS;
}

int put_uint64_les_into_buffer(struct buffer *buffer,
			       size_t count,
			       const uint64_t *ui)
{
	unsigned int i;

	if (!ensure_available_space(buffer, sizeof(uint64_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	for (i = 0; i < count; i++) {
		encode_uint64_le(buffer->data, &buffer->end, ui[i]);
	}

	return UDS_SUCCESS;
}
