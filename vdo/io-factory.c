// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/mount.h>

#include "io-factory.h"
#include "logger.h"
#include "memory-alloc.h"

enum { BLK_FMODE = FMODE_READ | FMODE_WRITE };

/*
 * A kernel mode IO Factory object controls access to an index stored
 * on a block device.
 */
struct io_factory {
	struct block_device *bdev;
	atomic_t ref_count;
};

void get_uds_io_factory(struct io_factory *factory)
{
	atomic_inc(&factory->ref_count);
}

static int get_block_device_from_name(const char *name,
				      struct block_device **bdev)
{
	dev_t device = name_to_dev_t(name);

	if (device != 0) {
		*bdev = blkdev_get_by_dev(device, BLK_FMODE, NULL);
	} else {
		*bdev = blkdev_get_by_path(name, BLK_FMODE, NULL);
	}
	if (IS_ERR(*bdev)) {
		uds_log_error_strerror(-PTR_ERR(*bdev),
				       "%s is not a block device", name);
		return UDS_INVALID_ARGUMENT;
	}

	return UDS_SUCCESS;
}

int make_uds_io_factory(const char *path, struct io_factory **factory_ptr)
{
	int result;
	struct block_device *bdev;
	struct io_factory *factory;

	result = get_block_device_from_name(path, &bdev);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(1, struct io_factory, __func__, &factory);
	if (result != UDS_SUCCESS) {
		blkdev_put(bdev, BLK_FMODE);
		return result;
	}

	factory->bdev = bdev;
	atomic_set_release(&factory->ref_count, 1);

	*factory_ptr = factory;
	return UDS_SUCCESS;
}

int replace_uds_storage(struct io_factory *factory, const char *path)
{
	int result;
	struct block_device *bdev;

	result = get_block_device_from_name(path, &bdev);
	if (result != UDS_SUCCESS) {
		return result;
	}

	blkdev_put(factory->bdev, BLK_FMODE);
	factory->bdev = bdev;
	return UDS_SUCCESS;
}

void put_uds_io_factory(struct io_factory *factory)
{
	if (atomic_add_return(-1, &factory->ref_count) <= 0) {
		blkdev_put(factory->bdev, BLK_FMODE);
		UDS_FREE(factory);
	}
}

size_t get_uds_writable_size(struct io_factory *factory)
{
	return i_size_read(factory->bdev->bd_inode);
}

int make_uds_bufio(struct io_factory *factory,
		   off_t offset,
		   size_t block_size,
		   unsigned int reserved_buffers,
		   struct dm_bufio_client **client_ptr)
{
	struct dm_bufio_client *client;

	if (offset % SECTOR_SIZE != 0) {
		return uds_log_error_strerror(UDS_INCORRECT_ALIGNMENT,
					      "offset %zd not multiple of %d",
					      offset,
					      SECTOR_SIZE);
	}
	if (block_size % UDS_BLOCK_SIZE != 0) {
		return uds_log_error_strerror(
			UDS_INCORRECT_ALIGNMENT,
			"block_size %zd not multiple of %d",
			block_size,
			UDS_BLOCK_SIZE);
	}

	client = dm_bufio_client_create(
		factory->bdev, block_size, reserved_buffers, 0, NULL, NULL);
	if (IS_ERR(client)) {
		return -PTR_ERR(client);
	}

	dm_bufio_set_sector_offset(client, offset >> SECTOR_SHIFT);
	*client_ptr = client;
	return UDS_SUCCESS;
}

int open_uds_buffered_reader(struct io_factory *factory,
			     off_t offset,
			     size_t size,
			     struct buffered_reader **reader_ptr)
{
	int result;
	struct dm_bufio_client *client = NULL;

	if (size % UDS_BLOCK_SIZE != 0) {
		return uds_log_error_strerror(
			UDS_INCORRECT_ALIGNMENT,
			"region size %zd is not multiple of %d",
			size,
			UDS_BLOCK_SIZE);
	}

	result = make_uds_bufio(factory, offset, UDS_BLOCK_SIZE, 1, &client);
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

int open_uds_buffered_writer(struct io_factory *factory,
			     off_t offset,
			     size_t size,
			     struct buffered_writer **writer_ptr)
{
	int result;
	struct dm_bufio_client *client = NULL;

	if (size % UDS_BLOCK_SIZE != 0) {
		return uds_log_error_strerror(UDS_INCORRECT_ALIGNMENT,
					      "region size %zd is not multiple of %d",
					      size,
					      UDS_BLOCK_SIZE);
	}

	result = make_uds_bufio(factory, offset, UDS_BLOCK_SIZE, 1, &client);
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
