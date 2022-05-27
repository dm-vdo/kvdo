/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
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
