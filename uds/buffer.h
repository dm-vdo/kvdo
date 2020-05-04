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
 * $Id: //eng/uds-releases/krusty/src/uds/buffer.h#4 $
 */

#ifndef BUFFER_H
#define BUFFER_H

#include "common.h"

struct buffer;

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
int __must_check wrap_buffer(byte *bytes,
			     size_t length,
			     size_t content_length,
			     struct buffer **buffer_ptr);

/**
 * Create a new buffer and allocate its memory.
 *
 * @param length     The length of the buffer
 * @param buffer_ptr A pointer to hold the buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_buffer(size_t length, struct buffer **buffer_ptr);

/**
 * Release a buffer and, if not wrapped, free its memory.
 *
 * @param p_buffer Pointer to the buffer to release
 **/
void free_buffer(struct buffer **p_buffer);

/**
 * Grow a non-wrapped buffer.
 *
 * @param buffer The buffer to resize
 * @param length The new length of the buffer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check grow_buffer(struct buffer *buffer, size_t length);

/**
 * Ensure that a buffer has a given amount of space available, compacting the
 * buffer if necessary.
 *
 * @param buffer The buffer
 * @param bytes  The number of available bytes desired
 *
 * @return <code>true</code> if the requested number of bytes are now available
 **/
bool __must_check ensure_available_space(struct buffer *buffer, size_t bytes);

/**
 * Clear the buffer. The start position is set to zero and the end position
 * is set to the buffer length.
 **/
void clear_buffer(struct buffer *buffer);

/**
 * Eliminate buffer contents which have been extracted. This function copies
 * any data between the start and end pointers to the beginning of the buffer,
 * moves the start pointer to the beginning, and the end pointer to the end
 * of the copied data.
 *
 * @param buffer The buffer to compact
 **/
void compact_buffer(struct buffer *buffer);

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
int __must_check skip_forward(struct buffer *buffer, size_t bytes_to_skip);

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
int __must_check rewind_buffer(struct buffer *buffer, size_t bytes_to_rewind);

/**
 * Return the length of the buffer.
 *
 * @param buffer        the buffer
 *
 * @return the buffer length
 **/
size_t buffer_length(struct buffer *buffer);

/**
 * Compute the amount of data current in the buffer.
 *
 * @param buffer The buffer to examine
 *
 * @return The number of bytes between the start and end pointers of the buffer
 **/
size_t content_length(struct buffer *buffer);

/**
 * Compute the amount of available space in this buffer.
 *
 * @param buffer The buffer to examine
 *
 * @return The number of bytes between the end pointer and the end of the buffer
 **/
size_t available_space(struct buffer *buffer);

/**
 * Amount of buffer that has already been processed.
 *
 * @param buffer  the buffer to examine
 *
 * @return The number of bytes between the beginning of the buffer and the
 *         start pointer.
 **/
size_t uncompacted_amount(struct buffer *buffer);

/**
 * Return the amount of the buffer that is currently utilized.
 *
 * @param buffer  the buffer to examine
 *
 * @return The number of bytes between the beginning of the buffer and
 *         the end pointer.
 **/
size_t buffer_used(struct buffer *buffer);

/**
 * Reset the end of buffer to a different position.
 *
 * @param buffer        the buffer
 * @param end           the new end of the buffer
 *
 * @return UDS_SUCCESS unless the end is larger than can fit
 **/
int __must_check reset_buffer_end(struct buffer *buffer, size_t end);

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
bool __must_check
has_same_bytes(struct buffer *buffer, const byte *data, size_t length);

/**
 * Check whether two buffers have the same contents.
 *
 * @param buffer1  The first buffer
 * @param buffer2  The second buffer
 *
 * @return <code>true</code> if the contents of the two buffers are the
 * same
 **/
bool equal_buffers(struct buffer *buffer1, struct buffer *buffer2);

/**
 * Get a single byte from a buffer and advance the start pointer.
 *
 * @param buffer   The buffer
 * @param byte_ptr A pointer to hold the byte
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there are no bytes to
 *         retrieve
 **/
int __must_check get_byte(struct buffer *buffer, byte *byte_ptr);

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
int __must_check
peek_byte(struct buffer *buffer, size_t offset, byte *byte_ptr);

/**
 * Put a single byte into a buffer and advance the end pointer.
 *
 * @param buffer  The buffer
 * @param b       The byte to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is no space in the buffer
 **/
int __must_check put_byte(struct buffer *buffer, byte b);

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
int __must_check
get_bytes_from_buffer(struct buffer *buffer, size_t length, void *destination);

/**
 * Get a pointer to the current contents of the buffer. This will be a pointer
 * to the actual memory managed by the buffer. It is the caller's responsibility
 * to ensure that the buffer is not modified while this pointer is in use.
 *
 * @param buffer The buffer from which to get the contents
 *
 * @return a pointer to the current contents of the buffer
 **/
byte *get_buffer_contents(struct buffer *buffer);

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
int __must_check
copy_bytes(struct buffer *buffer, size_t length, byte **destination_ptr);

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
int __must_check
put_bytes(struct buffer *buffer, size_t length, const void *source);

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
int __must_check
put_buffer(struct buffer *target, struct buffer *source, size_t length);

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
int __must_check zero_bytes(struct buffer *buffer, size_t length);

/**
 * Get a boolean value from a buffer and advance the start pointer.
 *
 * @param buffer The buffer
 * @param b      A pointer to hold the boolean value
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is not enough data
 *         in the buffer
 **/
int __must_check get_boolean(struct buffer *buffer, bool *b);

/**
 * Put a boolean value into a buffer and advance the end pointer.
 *
 * @param buffer  The buffer
 * @param b       The boolean to put
 *
 * @return UDS_SUCCESS or UDS_BUFFER_ERROR if there is no space in the buffer
 **/
int __must_check put_boolean(struct buffer *buffer, bool b);

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
int __must_check get_uint16_be_from_buffer(struct buffer *buffer, uint16_t *ui);

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
int __must_check put_uint16_be_into_buffer(struct buffer *buffer, uint16_t ui);

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
int __must_check get_uint32_be_from_buffer(struct buffer *buffer, uint32_t *ui);

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
int __must_check put_uint32_be_into_buffer(struct buffer *buffer, uint32_t ui);

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
int __must_check
get_uint32_bes_from_buffer(struct buffer *buffer, size_t count, uint32_t *ui);

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
int __must_check
put_uint32_bes_into_buffer(struct buffer *buffer,
			   size_t count,
			   const uint32_t *ui);

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
int __must_check
get_uint64_bes_from_buffer(struct buffer *buffer, size_t count, uint64_t *ui);

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
int __must_check
put_uint64_bes_into_buffer(struct buffer *buffer,
			   size_t count,
			   const uint64_t *ui);

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
int __must_check get_uint16_le_from_buffer(struct buffer *buffer, uint16_t *ui);

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
int __must_check put_uint16_le_into_buffer(struct buffer *buffer, uint16_t ui);

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
int __must_check
get_uint16_les_from_buffer(struct buffer *buffer, size_t count, uint16_t *ui);

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
int __must_check
put_uint16_les_into_buffer(struct buffer *buffer,
			   size_t count,
			   const uint16_t *ui);

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
int __must_check get_int32_le_from_buffer(struct buffer *buffer, int32_t *i);

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
int __must_check get_uint32_le_from_buffer(struct buffer *buffer, uint32_t *ui);

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
int __must_check put_uint32_le_into_buffer(struct buffer *buffer, uint32_t ui);

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
int __must_check get_uint64_le_from_buffer(struct buffer *buffer, uint64_t *ui);

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
int __must_check put_int64_le_into_buffer(struct buffer *buffer, int64_t i);

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
int __must_check put_uint64_le_into_buffer(struct buffer *buffer, uint64_t ui);

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
int __must_check
get_uint64_les_from_buffer(struct buffer *buffer, size_t count, uint64_t *ui);

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
int __must_check
put_uint64_les_into_buffer(struct buffer *buffer,
			   size_t count,
			   const uint64_t *ui);

#endif /* BUFFER_H */
