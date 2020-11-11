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
 *
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/ioFactoryLinuxKernel.c#7 $
 */

#include <linux/blkdev.h>
#include <linux/mount.h>

#include "atomicDefs.h"
#include "ioFactory.h"
#include "logger.h"
#include "memoryAlloc.h"

enum { BLK_FMODE = FMODE_READ | FMODE_WRITE };

/*
 * A kernel mode IO Factory object controls access to an index stored
 * on a block device.
 */
struct io_factory {
	struct block_device *bdev;
	atomic_t ref_count;
};

/*****************************************************************************/
void get_io_factory(struct io_factory *factory)
{
	atomic_inc(&factory->ref_count);
}

/*****************************************************************************/
int make_io_factory(const char *path, struct io_factory **factory_ptr)
{
	struct block_device *bdev;
	dev_t device = name_to_dev_t(path);
	if (device != 0) {
		bdev = blkdev_get_by_dev(device, BLK_FMODE, NULL);
	} else {
		bdev = blkdev_get_by_path(path, BLK_FMODE, NULL);
	}
	if (IS_ERR(bdev)) {
		log_error_strerror(-PTR_ERR(bdev),
				   "%s is not a block device", path);
		return UDS_INVALID_ARGUMENT;
	}

	struct io_factory *factory;
	int result = ALLOCATE(1, struct io_factory, __func__, &factory);
	if (result != UDS_SUCCESS) {
		blkdev_put(bdev, BLK_FMODE);
		return result;
	}

	factory->bdev = bdev;
	atomic_set_release(&factory->ref_count, 1);

	*factory_ptr = factory;
	return UDS_SUCCESS;
}

/*****************************************************************************/
void put_io_factory(struct io_factory *factory)
{
	if (atomic_add_return(-1, &factory->ref_count) <= 0) {
		blkdev_put(factory->bdev, BLK_FMODE);
		FREE(factory);
	}
}

/*****************************************************************************/
size_t get_writable_size(struct io_factory *factory)
{
	return i_size_read(factory->bdev->bd_inode);
}

/*****************************************************************************/
int make_bufio(struct io_factory *factory,
	       off_t offset,
	       size_t block_size,
	       unsigned int reserved_buffers,
	       struct dm_bufio_client **client_ptr)
{
	if (offset % SECTOR_SIZE != 0) {
		return log_error_strerror(UDS_INCORRECT_ALIGNMENT,
					  "offset %zd not multiple of %d",
					  offset,
					  SECTOR_SIZE);
	}
	if (block_size % UDS_BLOCK_SIZE != 0) {
		return log_error_strerror(
			UDS_INCORRECT_ALIGNMENT,
			"block_size %zd not multiple of %d",
			block_size,
			UDS_BLOCK_SIZE);
	}

	struct dm_bufio_client *client = dm_bufio_client_create(
		factory->bdev, block_size, reserved_buffers, 0, NULL, NULL);
	if (IS_ERR(client)) {
		return -PTR_ERR(client);
	}

	dm_bufio_set_sector_offset(client, offset >> SECTOR_SHIFT);
	*client_ptr = client;
	return UDS_SUCCESS;
}

/*****************************************************************************/
int open_buffered_reader(struct io_factory *factory,
			 off_t offset,
			 size_t size,
			 struct buffered_reader **reader_ptr)
{
	if (size % UDS_BLOCK_SIZE != 0) {
		return log_error_strerror(
			UDS_INCORRECT_ALIGNMENT,
			"region size %zd is not multiple of %d",
			size,
			UDS_BLOCK_SIZE);
	}

	struct dm_bufio_client *client = NULL;
	int result = make_bufio(factory, offset, UDS_BLOCK_SIZE, 1, &client);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = make_buffered_reader(
		factory, client, size / UDS_BLOCK_SIZE, reader_ptr);
	if (result != UDS_SUCCESS) {
		dm_bufio_client_destroy(client);
	}
	return result;
}

/*****************************************************************************/
int open_buffered_writer(struct io_factory *factory,
			 off_t offset,
			 size_t size,
			 struct buffered_writer **writer_ptr)
{
	if (size % UDS_BLOCK_SIZE != 0) {
		return log_error_strerror(UDS_INCORRECT_ALIGNMENT,
					  "region size %zd is not multiple of %d",
					  size,
					  UDS_BLOCK_SIZE);
	}

	struct dm_bufio_client *client = NULL;
	int result = make_bufio(factory, offset, UDS_BLOCK_SIZE, 1, &client);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = make_buffered_writer(
		factory, client, size / UDS_BLOCK_SIZE, writer_ptr);
	if (result != UDS_SUCCESS) {
		dm_bufio_client_destroy(client);
	}
	return result;
}
