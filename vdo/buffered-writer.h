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

#ifndef BUFFERED_WRITER_H
#define BUFFERED_WRITER_H 1

#include "common.h"

struct buffered_writer;
struct dm_bufio_client;
struct io_factory;

int __must_check make_buffered_writer(struct io_factory *factory,
				      struct dm_bufio_client *client,
				      sector_t block_limit,
				      struct buffered_writer **writer_ptr);

void free_buffered_writer(struct buffered_writer *buffer);

int __must_check write_to_buffered_writer(struct buffered_writer *writer,
					  const void *data,
					  size_t len);

int __must_check write_zeros_to_buffered_writer(struct buffered_writer *writer,
						size_t len);

int __must_check flush_buffered_writer(struct buffered_writer *writer);

#endif /* BUFFERED_WRITER_H */
