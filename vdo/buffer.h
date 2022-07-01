/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef BUFFER_H
#define BUFFER_H

#include "common.h"

struct buffer {
	size_t start;
	size_t end;
	size_t length;
	byte *data;
	bool wrapped;
};

int __must_check wrap_buffer(byte *bytes,
			     size_t length,
			     size_t content_length,
			     struct buffer **buffer_ptr);

int __must_check make_buffer(size_t length, struct buffer **buffer_ptr);
void free_buffer(struct buffer *buffer);

bool __must_check ensure_available_space(struct buffer *buffer, size_t bytes);

void clear_buffer(struct buffer *buffer);
void compact_buffer(struct buffer *buffer);
int __must_check skip_forward(struct buffer *buffer, size_t bytes_to_skip);
int __must_check rewind_buffer(struct buffer *buffer, size_t bytes_to_rewind);

size_t buffer_length(struct buffer *buffer);
size_t content_length(struct buffer *buffer);
size_t available_space(struct buffer *buffer);
size_t uncompacted_amount(struct buffer *buffer);
size_t buffer_used(struct buffer *buffer);

int __must_check reset_buffer_end(struct buffer *buffer, size_t end);

bool __must_check
has_same_bytes(struct buffer *buffer, const byte *data, size_t length);
bool equal_buffers(struct buffer *buffer1, struct buffer *buffer2);

int __must_check get_byte(struct buffer *buffer, byte *byte_ptr);
int __must_check put_byte(struct buffer *buffer, byte b);

int __must_check
get_bytes_from_buffer(struct buffer *buffer, size_t length, void *destination);
byte *get_buffer_contents(struct buffer *buffer);
int __must_check
copy_bytes(struct buffer *buffer, size_t length, byte **destination_ptr);
int __must_check
put_bytes(struct buffer *buffer, size_t length, const void *source);
int __must_check
put_buffer(struct buffer *target, struct buffer *source, size_t length);

int __must_check zero_bytes(struct buffer *buffer, size_t length);

int __must_check get_boolean(struct buffer *buffer, bool *b);
int __must_check put_boolean(struct buffer *buffer, bool b);

int __must_check get_uint16_le_from_buffer(struct buffer *buffer, uint16_t *ui);
int __must_check put_uint16_le_into_buffer(struct buffer *buffer, uint16_t ui);

int __must_check
get_uint16_les_from_buffer(struct buffer *buffer, size_t count, uint16_t *ui);
int __must_check
put_uint16_les_into_buffer(struct buffer *buffer,
			   size_t count,
			   const uint16_t *ui);

int __must_check get_int32_le_from_buffer(struct buffer *buffer, int32_t *i);
int __must_check get_uint32_le_from_buffer(struct buffer *buffer, uint32_t *ui);
int __must_check put_uint32_le_into_buffer(struct buffer *buffer, uint32_t ui);

int __must_check get_uint64_le_from_buffer(struct buffer *buffer, uint64_t *ui);
int __must_check put_int64_le_into_buffer(struct buffer *buffer, int64_t i);
int __must_check put_uint64_le_into_buffer(struct buffer *buffer, uint64_t ui);

int __must_check
get_uint64_les_from_buffer(struct buffer *buffer, size_t count, uint64_t *ui);
int __must_check
put_uint64_les_into_buffer(struct buffer *buffer,
			   size_t count,
			   const uint64_t *ui);

#endif /* BUFFER_H */
