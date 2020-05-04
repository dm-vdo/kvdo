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
 * $Id: //eng/uds-releases/krusty/src/uds/bufferedReader.h#3 $
 */

#ifndef BUFFERED_READER_H
#define BUFFERED_READER_H 1

#include "common.h"

struct dm_bufio_client;
struct ioFactory;

/**
 * The buffered reader allows efficient IO for IORegions, which may be
 * file- or block-based. The internal buffer always reads aligned data
 * from the underlying region.
 **/
typedef struct bufferedReader BufferedReader;

/**
 * Make a new buffered reader.
 *
 * @param factory      The IOFactory creating the buffered reader.
 * @param client       The dm_bufio_client to read from.
 * @param block_limit  The number of blocks that may be read.
 * @param reader_ptr   The pointer to hold the newly allocated buffered reader
 *
 * @return UDS_SUCCESS or error code.
 **/
int __must_check make_buffered_reader(struct ioFactory *factory,
				      struct dm_bufio_client *client,
				      sector_t block_limit,
				      BufferedReader **reader_ptr);

/**
 * Free a buffered reader.
 *
 * @param reader        The buffered reader
 **/
void free_buffered_reader(BufferedReader *reader);

/**
 * Retrieve data from a buffered reader, reading from the region when needed.
 *
 * @param reader        The buffered reader
 * @param data          The buffer to read data into
 * @param length        The length of the data to read
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
read_from_buffered_reader(BufferedReader *reader, void *data, size_t length);

/**
 * Verify that the data currently in the buffer matches the required value.
 *
 * @param reader        The buffered reader.
 * @param value         The value that must match the buffer contents.
 * @param length        The length of the value that must match.
 *
 * @return UDS_SUCCESS or an error code, specifically UDS_CORRUPT_FILE
 *         if the required value fails to match.
 *
 * @note If the value matches, the matching contents are consumed. However,
 *       if the match fails, any buffer contents are left as is.
 **/
int __must_check
verify_buffered_data(BufferedReader *reader, const void *value, size_t length);

#endif // BUFFERED_READER_H
