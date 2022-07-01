/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef BUFFERED_READER_H
#define BUFFERED_READER_H 1

#include "common.h"

struct buffered_reader;
struct dm_bufio_client;
struct io_factory;

int __must_check make_buffered_reader(struct io_factory *factory,
				      struct dm_bufio_client *client,
				      sector_t block_limit,
				      struct buffered_reader **reader_ptr);

void free_buffered_reader(struct buffered_reader *reader);

int __must_check read_from_buffered_reader(struct buffered_reader *reader,
					   void *data,
					   size_t length);

int __must_check verify_buffered_data(struct buffered_reader *reader,
				      const void *value,
				      size_t length);

#endif /* BUFFERED_READER_H */
