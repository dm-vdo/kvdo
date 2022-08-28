/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef IO_FACTORY_H
#define IO_FACTORY_H

#include "buffered-reader.h"
#include "buffered-writer.h"
#include <linux/dm-bufio.h>

/*
 * An IO factory object is responsible for controlling access to index
 * storage.  The index is a contiguous range of blocks on a block
 * device or within a file.
 *
 * The IO factory holds the open device or file and is responsible for
 * closing it.  The IO factory has methods to make IO regions that are
 * used to access sections of the index.
 */
struct io_factory;

/*
 * Define the UDS block size as 4K.  Historically, we wrote the volume file in
 * large blocks, but wrote all the other index data into byte streams stored in
 * files.  When we converted to writing an index into a block device, we
 * changed to writing the byte streams into page sized blocks.  Now that we
 * support multiple architectures, we write into 4K blocks on all platforms.
 *
 * XXX We must convert all the rogue 4K constants to use UDS_BLOCK_SIZE.
 */
enum { UDS_BLOCK_SIZE = 4096 };

/**
 * Create an IO factory. The IO factory is returned with a reference
 * count of 1.
 *
 * @param path        The path to the block device or file that contains the
 *                    block stream
 * @param factory_ptr The IO factory is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_uds_io_factory(const char *path,
				     struct io_factory **factory_ptr);

/**
 * Replace the backing store for an IO factory.
 *
 * @param factory  The IO factory
 * @param path     The path to the new block device or storage file
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check replace_uds_storage(struct io_factory *factory,
				     const char *path);

/**
 * Get another reference to an IO factory, incrementing its reference count.
 *
 * @param factory  The IO factory
 **/
void get_uds_io_factory(struct io_factory *factory);

/**
 * Free a reference to an IO factory.  If the reference count drops to zero,
 * free the IO factory and release all its resources.
 *
 * @param factory  The IO factory
 **/
void put_uds_io_factory(struct io_factory *factory);

/**
 * Get the maximum potential size of the device or file.  For a device, this is
 * the actual size of the device.  For a file, this is the largest file that we
 * can possibly write.
 *
 * @param factory  The IO factory
 *
 * @return the writable size (in bytes)
 **/
size_t __must_check get_uds_writable_size(struct io_factory *factory);

/**
 * Create a struct dm_bufio_client for a region of the index.
 *
 * @param factory          The IO factory
 * @param offset           The byte offset to the region within the index
 * @param block_size       The size of a block, in bytes
 * @param reserved_buffers The number of buffers that can be reserved
 * @param client_ptr       The struct dm_bufio_client is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_uds_bufio(struct io_factory *factory,
				off_t offset,
				size_t block_size,
				unsigned int reserved_buffers,
				struct dm_bufio_client **client_ptr);

/**
 * Create a buffered reader for a region of the index.
 *
 * @param factory    The IO factory
 * @param offset     The byte offset to the region within the index
 * @param size       The size in bytes of the region
 * @param reader_ptr The buffered reader is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check open_uds_buffered_reader(struct io_factory *factory,
					  off_t offset,
					  size_t size,
					  struct buffered_reader **reader_ptr);

/**
 * Create a buffered writer for a region of the index.
 *
 * @param factory    The IO factory
 * @param offset     The byte offset to the region within the index
 * @param size       The size in bytes of the region
 * @param writer_ptr The buffered writer is returned here
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check open_uds_buffered_writer(struct io_factory *factory,
					  off_t offset,
					  size_t size,
					  struct buffered_writer **writer_ptr);

#endif /* IO_FACTORY_H */
