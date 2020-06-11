/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/krusty/src/uds/buffer.c#4 $
 */

#include "buffer.h"

#include "bufferPrivate.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"
#include "typeDefs.h"

/**********************************************************************/
int wrap_buffer(byte *bytes,
		size_t length,
		size_t content_length,
		struct buffer **buffer_ptr)
{
	int result = ASSERT((content_length <= length),
			    "content length, %zu, fits in buffer size, %zu",
			    length,
			    content_length);
	struct buffer *buffer;
	result = ALLOCATE(1, struct buffer, "buffer", &buffer);
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

/***********************************************************************/
int make_buffer(size_t size, struct buffer **new_buffer)
{
	byte *data;
	int result = ALLOCATE(size, byte, "buffer data", &data);
	if (result != UDS_SUCCESS) {
		return result;
	}

	struct buffer *buffer;
	result = wrap_buffer(data, size, 0, &buffer);
	if (result != UDS_SUCCESS) {
		FREE(data);
		return result;
	}

	buffer->wrapped = false;
	*new_buffer = buffer;
	return UDS_SUCCESS;
}

/***********************************************************************/
void free_buffer(struct buffer **p_buffer)
{
	struct buffer *buffer = *p_buffer;
	*p_buffer = NULL;
	if (buffer == NULL) {
		return;
	}
	if (!buffer->wrapped) {
		FREE(buffer->data);
	}
	FREE(buffer);
}

/**********************************************************************/
size_t buffer_length(struct buffer *buffer)
{
	return buffer->length;
}

/**********************************************************************/
size_t content_length(struct buffer *buffer)
{
	return buffer->end - buffer->start;
}

/**********************************************************************/
size_t uncompacted_amount(struct buffer *buffer)
{
	return buffer->start;
}

/**********************************************************************/
size_t available_space(struct buffer *buffer)
{
	return buffer->length - buffer->end;
}

/**********************************************************************/
size_t buffer_used(struct buffer *buffer)
{
	return buffer->end;
}

/***********************************************************************/
int grow_buffer(struct buffer *buffer, size_t length)
{
	if (buffer == NULL) {
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot resize NULL buffer");
	}

	if (buffer->wrapped) {
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot resize wrapped buffer");
	}
	if (buffer->end > length) {
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot shrink buffer");
	}

	byte *data;
	int result = reallocate_memory(buffer->data, buffer->length, length,
				       "buffer data", &data);
	if (result != UDS_SUCCESS) {
		return result;
	}

	buffer->data = data;
	buffer->length = length;
	return UDS_SUCCESS;
}

/***********************************************************************/
bool ensure_available_space(struct buffer *buffer, size_t bytes)
{
	if (available_space(buffer) >= bytes) {
		return true;
	}
	compact_buffer(buffer);
	return (available_space(buffer) >= bytes);
}

/***********************************************************************/
void clear_buffer(struct buffer *buffer)
{
	buffer->start = 0;
	buffer->end = buffer->length;
}

/***********************************************************************/
void compact_buffer(struct buffer *buffer)
{
	if ((buffer->start == 0) || (buffer->end == 0)) {
		return;
	}
	size_t bytes_to_move = buffer->end - buffer->start;
	memmove(buffer->data, buffer->data + buffer->start, bytes_to_move);
	buffer->start = 0;
	buffer->end = bytes_to_move;
}

/**********************************************************************/
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

/**********************************************************************/
int skip_forward(struct buffer *buffer, size_t bytes_to_skip)
{
	if (content_length(buffer) < bytes_to_skip) {
		return UDS_BUFFER_ERROR;
	}

	buffer->start += bytes_to_skip;
	return UDS_SUCCESS;
}

/**********************************************************************/
int rewind_buffer(struct buffer *buffer, size_t bytes_to_rewind)
{
	if (buffer->start < bytes_to_rewind) {
		return UDS_BUFFER_ERROR;
	}

	buffer->start -= bytes_to_rewind;
	return UDS_SUCCESS;
}

/**********************************************************************/
bool has_same_bytes(struct buffer *buffer, const byte *data, size_t length)
{
	return ((content_length(buffer) >= length) &&
		(memcmp(buffer->data + buffer->start, data, length) == 0));
}

/**********************************************************************/
bool equal_buffers(struct buffer *buffer1, struct buffer *buffer2)
{
	return has_same_bytes(buffer1,
			      buffer2->data + buffer2->start,
			      content_length(buffer2));
}

/**********************************************************************/
int get_byte(struct buffer *buffer, byte *byte_ptr)
{
	if (content_length(buffer) < sizeof(byte)) {
		return UDS_BUFFER_ERROR;
	}

	*byte_ptr = buffer->data[buffer->start++];
	return UDS_SUCCESS;
}

/**********************************************************************/
int peek_byte(struct buffer *buffer, size_t offset, byte *byte_ptr)
{
	if (content_length(buffer) < (offset + sizeof(byte))) {
		return UDS_BUFFER_ERROR;
	}

	*byte_ptr = buffer->data[buffer->start + offset];
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_byte(struct buffer *buffer, byte b)
{
	if (!ensure_available_space(buffer, sizeof(byte))) {
		return UDS_BUFFER_ERROR;
	}

	buffer->data[buffer->end++] = b;
	return UDS_SUCCESS;
}

/**********************************************************************/
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

/**********************************************************************/
byte *get_buffer_contents(struct buffer *buffer)
{
	return buffer->data + buffer->start;
}

/**********************************************************************/
int copy_bytes(struct buffer *buffer, size_t length, byte **destination_ptr)
{
	byte *destination;
	int result =
		ALLOCATE(length, byte, "copy_bytes() buffer", &destination);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_bytes_from_buffer(buffer, length, destination);
	if (result != UDS_SUCCESS) {
		FREE(destination);
	} else {
		*destination_ptr = destination;
	}
	return result;
}

/**********************************************************************/
int put_bytes(struct buffer *buffer, size_t length, const void *source)
{
	if (!ensure_available_space(buffer, length)) {
		return UDS_BUFFER_ERROR;
	}
	memcpy(buffer->data + buffer->end, source, length);
	buffer->end += length;
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_buffer(struct buffer *target, struct buffer *source, size_t length)
{
	if (content_length(source) < length) {
		return UDS_BUFFER_ERROR;
	}

	int result = put_bytes(target, length, get_buffer_contents(source));
	if (result != UDS_SUCCESS) {
		return result;
	}

	source->start += length;
	return UDS_SUCCESS;
}

/**********************************************************************/
int zero_bytes(struct buffer *buffer, size_t length)
{
	if (!ensure_available_space(buffer, length)) {
		return UDS_BUFFER_ERROR;
	}
	memset(buffer->data + buffer->end, 0, length);
	buffer->end += length;
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_boolean(struct buffer *buffer, bool *b)
{
	byte by;
	int result = get_byte(buffer, &by);
	if (result == UDS_SUCCESS) {
		*b = (by == 1);
	}
	return result;
}

/**********************************************************************/
int put_boolean(struct buffer *buffer, bool b)
{
	return put_byte(buffer, (byte)(b ? 1 : 0));
}

/**********************************************************************/
int get_uint16_be_from_buffer(struct buffer *buffer, uint16_t *ui)
{
	if (content_length(buffer) < sizeof(uint16_t)) {
		return UDS_BUFFER_ERROR;
	}

	decodeUInt16BE(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_uint16_be_into_buffer(struct buffer *buffer, uint16_t ui)
{
	if (!ensure_available_space(buffer, sizeof(uint16_t))) {
		return UDS_BUFFER_ERROR;
	}

	encodeUInt16BE(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_uint32_be_from_buffer(struct buffer *buffer, uint32_t *ui)
{
	if (content_length(buffer) < sizeof(uint32_t)) {
		return UDS_BUFFER_ERROR;
	}

	decodeUInt32BE(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_uint32_be_into_buffer(struct buffer *buffer, uint32_t ui)
{
	if (!ensure_available_space(buffer, sizeof(uint32_t))) {
		return UDS_BUFFER_ERROR;
	}

	encodeUInt32BE(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_uint32_bes_from_buffer(struct buffer *buffer, size_t count,
			       uint32_t *ui)
{
	if (content_length(buffer) < (sizeof(uint32_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	unsigned int i;
	for (i = 0; i < count; i++) {
		decodeUInt32BE(buffer->data, &buffer->start, ui + i);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_uint32_bes_into_buffer(struct buffer *buffer,
				 size_t count,
				 const uint32_t *ui)
{
	if (!ensure_available_space(buffer, sizeof(uint32_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	unsigned int i;
	for (i = 0; i < count; i++) {
		encodeUInt32BE(buffer->data, &buffer->end, ui[i]);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_uint64_bes_from_buffer(struct buffer *buffer, size_t count,
			       uint64_t *ui)
{
	if (content_length(buffer) < (sizeof(uint64_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	unsigned int i;
	for (i = 0; i < count; i++) {
		decodeUInt64BE(buffer->data, &buffer->start, ui + i);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_uint64_bes_into_buffer(struct buffer *buffer,
				 size_t count,
				 const uint64_t *ui)
{
	if (!ensure_available_space(buffer, sizeof(uint64_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	unsigned int i;
	for (i = 0; i < count; i++) {
		encodeUInt64BE(buffer->data, &buffer->end, ui[i]);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_uint16_le_from_buffer(struct buffer *buffer, uint16_t *ui)
{
	if (content_length(buffer) < sizeof(uint16_t)) {
		return UDS_BUFFER_ERROR;
	}

	decodeUInt16LE(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_uint16_le_into_buffer(struct buffer *buffer, uint16_t ui)
{
	if (!ensure_available_space(buffer, sizeof(uint16_t))) {
		return UDS_BUFFER_ERROR;
	}

	encodeUInt16LE(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_uint16_les_from_buffer(struct buffer *buffer, size_t count,
			       uint16_t *ui)
{
	if (content_length(buffer) < (sizeof(uint16_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	unsigned int i;
	for (i = 0; i < count; i++) {
		decodeUInt16LE(buffer->data, &buffer->start, ui + i);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_uint16_les_into_buffer(struct buffer *buffer,
				 size_t count,
				 const uint16_t *ui)
{
	if (!ensure_available_space(buffer, sizeof(uint16_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	unsigned int i;
	for (i = 0; i < count; i++) {
		encodeUInt16LE(buffer->data, &buffer->end, ui[i]);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_int32_le_from_buffer(struct buffer *buffer, int32_t *i)
{
	if (content_length(buffer) < sizeof(int32_t)) {
		return UDS_BUFFER_ERROR;
	}

	decodeInt32LE(buffer->data, &buffer->start, i);
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_uint32_le_from_buffer(struct buffer *buffer, uint32_t *ui)
{
	if (content_length(buffer) < sizeof(uint32_t)) {
		return UDS_BUFFER_ERROR;
	}

	decodeUInt32LE(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_uint32_le_into_buffer(struct buffer *buffer, uint32_t ui)
{
	if (!ensure_available_space(buffer, sizeof(uint32_t))) {
		return UDS_BUFFER_ERROR;
	}

	encodeUInt32LE(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_int64_le_into_buffer(struct buffer *buffer, int64_t i)
{
	if (!ensure_available_space(buffer, sizeof(int64_t))) {
		return UDS_BUFFER_ERROR;
	}

	encodeInt64LE(buffer->data, &buffer->end, i);
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_uint64_le_from_buffer(struct buffer *buffer, uint64_t *ui)
{
	if (content_length(buffer) < sizeof(uint64_t)) {
		return UDS_BUFFER_ERROR;
	}

	decodeUInt64LE(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_uint64_le_into_buffer(struct buffer *buffer, uint64_t ui)
{
	if (!ensure_available_space(buffer, sizeof(uint64_t))) {
		return UDS_BUFFER_ERROR;
	}

	encodeUInt64LE(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_uint64_les_from_buffer(struct buffer *buffer, size_t count,
			       uint64_t *ui)
{
	if (content_length(buffer) < (sizeof(uint64_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	unsigned int i;
	for (i = 0; i < count; i++) {
		decodeUInt64LE(buffer->data, &buffer->start, ui + i);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int put_uint64_les_into_buffer(struct buffer *buffer,
				 size_t count,
				 const uint64_t *ui)
{
	if (!ensure_available_space(buffer, sizeof(uint64_t) * count)) {
		return UDS_BUFFER_ERROR;
	}

	unsigned int i;
	for (i = 0; i < count; i++) {
		encodeUInt64LE(buffer->data, &buffer->end, ui[i]);
	}
	return UDS_SUCCESS;
}
