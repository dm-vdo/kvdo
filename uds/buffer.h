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
 * $Id: //eng/uds-releases/krusty/src/uds/buffer.h#2 $
 */

#ifndef BUFFER_H
#define BUFFER_H

#include "common.h"

typedef struct buffer Buffer;

/**
 * Create a buffer which wraps an existing byte array.
 *
 * @param bytes          The bytes to wrap
 * @param length         The length of the buffer
 * @param content_length The length of the current contents of the buffer
 * @param buffer_ptr     A pointer to hold the buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int wrap_buffer(byte *bytes,
		size_t length,
		size_t content_length,
		Buffer **buffer_ptr) __attribute__((warn_unused_result));

/**
 * Create a new buffer and allocate its memory.
 *
 * @param length     The length of the buffer
 * @param buffer_ptr A pointer to hold the buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int make_buffer(size_t length, Buffer **buffer_ptr)
	__attribute__((warn_unused_result));

/**
 * Release a buffer and, if not wrapped, free its memory.
 *
 * @param p_buffer Pointer to the buffer to release
 **/
void free_buffer(Buffer **p_buffer);

/**
 * Grow a non-wrapped buffer.
 *
 * @param buffer The buffer to resize
 * @param length The new length of the buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int grow_buffer(Buffer *buffer, size_t length)
	__attribute__((warn_unused_result));

/**
 * Ensure that a buffer has a given amount of space available, compacting the
 * buffer if necessary.
 *
 * @param buffer The buffer
 * @param bytes  The number of available bytes desired
 *
 * @return <code>true</code> if the requested number of bytes are now available
 **/
bool ensure_available_space(Buffer *buffer, size_t bytes)
	__attribute__((warn_unused_result));

/**
 * Clear the buffer. The start position is set to zero and the end position
 * is set to the buffer length.
 **/
void clear_buffer(Buffer *buffer);

/**
 * Eliminate buffer contents which have been extracted. This function copies
 * any data between the start and end pointers to the beginning of the buffer,
 * moves the start pointer to the beginning, and the end pointer to the end
 * of the copied data.
 *
 * @param buffer The buffer to compact
 **/
void compact_buffer(Buffer *buffer);

/**
 * Skip forward the specified number of bytes in a buffer (advance the
 * start pointer).
 *
 * @param buffer        The buffer
 * @param bytes_to_skip The number of bytes to skip
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if the buffer is not long
 *         enough to skip forward the requested number of bytes
 **/
int skip_forward(Buffer *buffer, size_t bytes_to_skip)
	__attribute__((warn_unused_result));

/**
 * Rewind the specified number of bytes in a buffer (back up the start
 * pointer).
 *
 * @param buffer          The buffer
 * @param bytes_to_rewind The number of bytes to rewind
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if the buffer is not long
 *         enough to rewind backward the requested number of bytes
 **/
int rewind_buffer(Buffer *buffer, size_t bytes_to_rewind)
	__attribute__((warn_unused_result));

/**
 * Return the length of the buffer.
 *
 * @param buffer        the buffer
 *
 * @return the buffer length
 **/
size_t buffer_length(Buffer *buffer);

/**
 * Compute the amount of data current in the buffer.
 *
 * @param buffer The buffer to examine
 *
 * @return The number of bytes between the start and end pointers of the buffer
 **/
size_t content_length(Buffer *buffer);

/**
 * Compute the amount of available space in this buffer.
 *
 * @param buffer The buffer to examine
 *
 * @return The number of bytes between the end pointer and the end of the buffer
 **/
size_t available_space(Buffer *buffer);

/**
 * Amount of buffer that has already been processed.
 *
 * @param buffer  the buffer to examine
 *
 * @return The number of bytes between the beginning of the buffer and the
 *         start pointer.
 **/
size_t uncompacted_amount(Buffer *buffer);

/**
 * Return the amount of the buffer that is currently utilized.
 *
 * @param buffer  the buffer to examine
 *
 * @return The number of bytes between the beginning of the buffer and
 *         the end pointer.
 **/
size_t buffer_used(Buffer *buffer);

/**
 * Reset the end of buffer to a different position.
 *
 * @param buffer        the buffer
 * @param end           the new end of the buffer
 *
 * @return UDS_SUCCESS unless the end is larger than can fit
 **/
int reset_buffer_end(Buffer *buffer, size_t end)
	__attribute__((warn_unused_result));

/**
 * Check whether the start of the content of a buffer matches a specified
 * array of bytes.
 *
 * @param buffer The buffer to check
 * @param data   The desired data
 * @param length The length of the desired data
 *
 * @return <code>true</code> if the first length bytes of the buffer's
 *         contents match data
 **/
bool has_same_bytes(Buffer *buffer, const byte *data, size_t length)
	__attribute__((warn_unused_result));

/**
 * Check whether two buffers have the same contents.
 *
 * @param buffer1  The first buffer
 * @param buffer2  The second buffer
 *
 * @return <code>true</code> if the contents of the two buffers are the
 * same
 **/
bool equal_buffers(Buffer *buffer1, Buffer *buffer2);

/**
 * Get a single byte from a buffer and advance the start pointer.
 *
 * @param buffer   The buffer
 * @param byte_ptr A pointer to hold the byte
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are no bytes to
 *         retrieve
 **/
int get_byte(Buffer *buffer, byte *byte_ptr)
	__attribute__((warn_unused_result));

/**
 * Get a single byte from a buffer without advancing the start pointer.
 *
 * @param buffer   The buffer
 * @param offset   The offset past the start pointer of the desired byte
 * @param byte_ptr A pointer to hold the byte
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if the offset is past the end
 * of the buffer
 **/
int peek_byte(Buffer *buffer, size_t offset, byte *byte_ptr)
	__attribute__((warn_unused_result));

/**
 * Put a single byte into a buffer and advance the end pointer.
 *
 * @param buffer  The buffer
 * @param b       The byte to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is no space in the buffer
 **/
int put_byte(Buffer *buffer, byte b) __attribute__((warn_unused_result));

/**
 * Get bytes out of a buffer and advance the start of the buffer past the
 * copied data.
 *
 * @param buffer      The buffer from which to copy
 * @param length      The number of bytes to copy
 * @param destination A pointer to hold the data
 *
 * @return UDS_SUCCESS or an error code
 **/
int get_bytes_from_buffer(Buffer *buffer, size_t length, void *destination)
	__attribute__((warn_unused_result));

/**
 * Get a pointer to the current contents of the buffer. This will be a pointer
 * to the actual memory managed by the buffer. It is the caller's responsibility
 * to ensure that the buffer is not modified while this pointer is in use.
 *
 * @param buffer The buffer from which to get the contents
 *
 * @return a pointer to the current contents of the buffer
 **/
byte *get_buffer_contents(Buffer *buffer);

/**
 * Copy bytes out of a buffer and advance the start of the buffer past the
 * copied data. Memory will be allocated to hold the copy.
 *
 * @param buffer          The buffer from which to copy
 * @param length          The number of bytes to copy
 * @param destination_ptr A pointer to hold the copied data
 *
 * @return UDS_SUCCESS or an error code
 **/
int copy_bytes(Buffer *buffer, size_t length, byte **destination_ptr)
	__attribute__((warn_unused_result));

/**
 * Copy bytes into a buffer and advance the end of the buffer past the
 * copied data.
 *
 * @param buffer The buffer to copy into
 * @param length The length of the data to copy
 * @param source The data to copy
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if the buffer does not have
 *         length bytes available
 **/
int put_bytes(Buffer *buffer, size_t length, const void *source)
	__attribute__((warn_unused_result));

/**
 * Copy the contents of a source buffer into the target buffer. Advances the
 * start of the source buffer and the end of the target buffer past the copied
 * data.
 *
 * @param target The buffer to receive the copy of the data
 * @param source The buffer containing the data to copy
 * @param length The length of the data to copy
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if the target buffer does not have
 *         length bytes available or if the source buffer does not have length
 *         bytes of content
 **/
int put_buffer(Buffer *target, Buffer *source, size_t length)
	__attribute__((warn_unused_result));

/**
 * Zero bytes in a buffer starting at the start pointer, and advance the
 * end of the buffer past the zeros.
 *
 * @param buffer The buffer to zero
 * @param length The number of bytes to zero
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if the buffer does not have
 *         length bytes available
 **/
int zero_bytes(Buffer *buffer, size_t length)
	__attribute__((warn_unused_result));

/**
 * Get a boolean value from a buffer and advance the start pointer.
 *
 * @param buffer The buffer
 * @param b      A pointer to hold the boolean value
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough data
 *         in the buffer
 **/
int get_boolean(Buffer *buffer, bool *b) __attribute__((warn_unused_result));

/**
 * Put a boolean value into a buffer and advance the end pointer.
 *
 * @param buffer  The buffer
 * @param b       The boolean to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is no space in the buffer
 **/
int put_boolean(Buffer *buffer, bool b) __attribute__((warn_unused_result));

/**
 * Get a 2 byte, big endian encoded integer from a buffer and advance the
 * start pointer past it.
 *
 * @param buffer The buffer
 * @param ui     A pointer to hold the integer
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 2
 *         bytes available
 **/
int get_uint16_be_from_buffer(Buffer *buffer, uint16_t *ui)
	__attribute__((warn_unused_result));

/**
 * Put a 2 byte, big endian encoded integer into a buffer and advance the
 * end pointer past it.
 *
 * @param buffer The buffer
 * @param ui     The integer to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 2
 *         bytes available
 **/
int put_uint16_be_into_buffer(Buffer *buffer, uint16_t ui)
	__attribute__((warn_unused_result));

/**
 * Get a 4 byte, big endian encoded integer from a buffer and advance the
 * start pointer past it.
 *
 * @param buffer The buffer
 * @param ui     A pointer to hold the integer
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 4
 *         bytes available
 **/
int get_uint32_be_from_buffer(Buffer *buffer, uint32_t *ui)
	__attribute__((warn_unused_result));

/**
 * Put a 4 byte, big endian encoded integer into a buffer and advance the
 * end pointer past it.
 *
 * @param buffer The buffer
 * @param ui     The integer to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 4
 *         bytes available
 **/
int put_uint32_be_into_buffer(Buffer *buffer, uint32_t ui)
	__attribute__((warn_unused_result));

/**
 * Get a series of 4 byte, big endian encoded integer from a buffer and
 * advance the start pointer past them.
 *
 * @param buffer The buffer
 * @param count  The number of integers to get
 * @param ui     A pointer to hold the integers
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough data
 *         in the buffer
 **/
int get_uint32_bes_from_buffer(Buffer *buffer, size_t count, uint32_t *ui)
	__attribute__((warn_unused_result));

/**
 * Put a series of 4 byte, big endian encoded integers into a buffer and
 * advance the end pointer past them.
 *
 * @param buffer The buffer
 * @param count  The number of integers to put
 * @param ui     A pointer to the integers
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough space
 *         in the buffer
 **/
int put_uint32_bes_into_buffer(Buffer *buffer,
			       size_t count,
			       const uint32_t *ui)
	__attribute__((warn_unused_result));

/**
 * Get a series of 8 byte, big endian encoded integer from a buffer and
 * advance the start pointer past them.
 *
 * @param buffer The buffer
 * @param count  The number of integers to get
 * @param ui     A pointer to hold the integers
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough data
 *         in the buffer
 **/
int get_uint64_bes_from_buffer(Buffer *buffer, size_t count, uint64_t *ui)
	__attribute__((warn_unused_result));

/**
 * Put a series of 8 byte, big endian encoded integers into a buffer and
 * advance the end pointer past them.
 *
 * @param buffer The buffer
 * @param count  The number of integers to put
 * @param ui     A pointer to the integers
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough space
 *         in the buffer
 **/
int put_uint64_bes_into_buffer(Buffer *buffer,
			       size_t count,
			       const uint64_t *ui)
	__attribute__((warn_unused_result));

/**
 * Get a 2 byte, little endian encoded integer from a buffer and
 * advance the start pointer past it.
 *
 * @param buffer The buffer
 * @param ui     A pointer to hold the integer
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 2
 *         bytes available
 **/
int get_uint16_le_from_buffer(Buffer *buffer, uint16_t *ui)
	__attribute__((warn_unused_result));

/**
 * Put a 2 byte, little endian encoded integer into a buffer and advance the
 * end pointer past it.
 *
 * @param buffer The buffer
 * @param ui     The integer to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 2
 *         bytes available
 **/
int put_uint16_le_into_buffer(Buffer *buffer, uint16_t ui)
	__attribute__((warn_unused_result));

/**
 * Get a series of 2 byte, little endian encoded integer from a buffer
 * and advance the start pointer past them.
 *
 * @param buffer The buffer
 * @param count  The number of integers to get
 * @param ui     A pointer to hold the integers
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough data
 *         in the buffer
 **/
int get_uint16_les_from_buffer(Buffer *buffer, size_t count, uint16_t *ui)
	__attribute__((warn_unused_result));

/**
 * Put a series of 2 byte, little endian encoded integers into a
 * buffer and advance the end pointer past them.
 *
 * @param buffer The buffer
 * @param count  The number of integers to put
 * @param ui     A pointer to the integers
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough space
 *         in the buffer
 **/
int put_uint16_les_into_buffer(Buffer *buffer,
			       size_t count,
			       const uint16_t *ui)
	__attribute__((warn_unused_result));

/**
 * Get a 4 byte, little endian encoded integer from a buffer and advance the
 * start pointer past it.
 *
 * @param buffer The buffer
 * @param i      A pointer to hold the integer
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 4
 *         bytes available
 **/
int get_int32_le_from_buffer(Buffer *buffer, int32_t *i)
	__attribute__((warn_unused_result));

/**
 * Get a 4 byte, little endian encoded integer from a buffer and advance the
 * start pointer past it.
 *
 * @param buffer The buffer
 * @param ui     A pointer to hold the integer
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 4
 *         bytes available
 **/
int get_uint32_le_from_buffer(Buffer *buffer, uint32_t *ui)
	__attribute__((warn_unused_result));

/**
 * Put a 4 byte, little endian encoded integer into a buffer and advance the
 * end pointer past it.
 *
 * @param buffer The buffer
 * @param ui     The integer to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 4
 *         bytes available
 **/
int put_uint32_le_into_buffer(Buffer *buffer, uint32_t ui)
	__attribute__((warn_unused_result));

/**
 * Get an 8 byte, little endian encoded, unsigned integer from a
 * buffer and advance the start pointer past it.
 *
 * @param buffer The buffer
 * @param ui     A pointer to hold the integer
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 8
 *         bytes available
 **/
int get_uint64_le_from_buffer(Buffer *buffer, uint64_t *ui)
	__attribute__((warn_unused_result));

/**
 * Put an 8 byte, little endian encoded signed integer into a buffer
 * and advance the end pointer past it.
 *
 * @param buffer The buffer
 * @param i      The integer to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 8
 *         bytes available
 **/
int put_int64_le_into_buffer(Buffer *buffer, int64_t i)
	__attribute__((warn_unused_result));

/**
 * Put an 8 byte, little endian encoded integer into a buffer and advance the
 * end pointer past it.
 *
 * @param buffer The buffer
 * @param ui     The integer to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are fewer than 8
 *         bytes available
 **/
int put_uint64_le_into_buffer(Buffer *buffer, uint64_t ui)
	__attribute__((warn_unused_result));

/**
 * Get a series of 8 byte, little endian encoded integer from a buffer
 * and advance the start pointer past them.
 *
 * @param buffer The buffer
 * @param count  The number of integers to get
 * @param ui     A pointer to hold the integers
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough data
 *         in the buffer
 **/
int get_uint64_les_from_buffer(Buffer *buffer, size_t count, uint64_t *ui)
	__attribute__((warn_unused_result));

/**
 * Put a series of 8 byte, little endian encoded integers into a buffer and
 * advance the end pointer past them.
 *
 * @param buffer The buffer
 * @param count  The number of integers to put
 * @param ui     A pointer to the integers
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough space
 *         in the buffer
 **/
int put_uint64_les_into_buffer(Buffer *buffer,
			       size_t count,
			       const uint64_t *ui)
	__attribute__((warn_unused_result));

#endif /* BUFFER_H */
