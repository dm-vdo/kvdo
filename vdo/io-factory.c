// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/mount.h>
#ifndef VDO_UPSTREAM
#include <linux/version.h>
#endif /* VDO_UPSTREAM */

#include "io-factory.h"
#include "logger.h"
#include "memory-alloc.h"
#ifndef VDO_UPSTREAM
#undef VDO_USE_ALTERNATE
#ifdef RHEL_RELEASE_CODE
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 4))
#define VDO_USE_ALTERNATE
#endif
#else /* !RHEL_RELEASE_CODE */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,5,0))
#define VDO_USE_ALTERNATE
#endif
#endif /* !RHEL_RELEASE_CODE */
#endif /* !VDO_UPSTREAM */

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
	dev_t device;
	unsigned int major, minor;
	char dummy;
#ifndef VDO_USE_ALTERNATE
	const struct blk_holder_ops hops = { NULL };
#endif /* !VDO_USE_ALTERNATE */

	/* Extract the major/minor numbers */
	if (sscanf(name, "%u:%u%c", &major, &minor, &dummy) == 2) {
		device = MKDEV(major, minor);
		if (MAJOR(device) != major || MINOR(device) != minor) {
			*bdev = NULL;
			return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
						      "%s is not a valid block device",
						      name);
		}
#ifdef VDO_USE_ALTERNATE
		*bdev = blkdev_get_by_dev(device, BLK_FMODE, NULL);
#else
		*bdev = blkdev_get_by_dev(device, BLK_FMODE, NULL, &hops);
#endif /* VDO_USE_ALTERNATE */
	} else {
#ifdef VDO_USE_ALTERNATE
		*bdev = blkdev_get_by_path(name, BLK_FMODE, NULL);
#else
		*bdev = blkdev_get_by_path(name, BLK_FMODE, NULL, &hops);
#endif /* VDO_USE_ALTERNATE */
	}

	if (IS_ERR(*bdev)) {
		uds_log_error_strerror(-PTR_ERR(*bdev), "%s is not a block device", name);
		*bdev = NULL;
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
#ifdef VDO_USE_ALTERNATE
		blkdev_put(bdev, BLK_FMODE);
#else
		blkdev_put(bdev, NULL);
#endif /* VDO_USE_ALTERNATE */
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

#ifdef VDO_USE_ALTERNATE
	blkdev_put(factory->bdev, BLK_FMODE);
#else
	blkdev_put(factory->bdev, NULL);
#endif /* VDO_USE_ALTERNATE */
	factory->bdev = bdev;
	return UDS_SUCCESS;
}

void put_uds_io_factory(struct io_factory *factory)
{
	if (atomic_add_return(-1, &factory->ref_count) <= 0) {
#ifdef VDO_USE_ALTERNATE
		blkdev_put(factory->bdev, BLK_FMODE);
#else
		blkdev_put(factory->bdev, NULL);
#endif /* VDO_USE_ALTERNATE */
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

#ifdef DM_BUFIO_CLIENT_NO_SLEEP
	client = dm_bufio_client_create(
		factory->bdev, block_size, reserved_buffers, 0, NULL, NULL, 0);
#else
	client = dm_bufio_client_create(
		factory->bdev, block_size, reserved_buffers, 0, NULL, NULL);
#endif
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
