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
#undef VDO_USE_ALTERNATE_2
#undef VDO_USE_ALTERNATE_3
#ifdef RHEL_RELEASE_CODE
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 5))
#define VDO_USE_ALTERNATE
#define VDO_USE_ALTERNATE_2
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 4))
#define VDO_USE_ALTERNATE_3
#endif
#endif
#else /* !RHEL_RELEASE_CODE */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,9,0))
#define VDO_USE_ALTERNATE
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,7,0))
#define VDO_USE_ALTERNATE_2
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,5,0))
#define VDO_USE_ALTERNATE_3
#endif
#endif
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
#ifdef VDO_USE_ALTERNATE
#ifndef VDO_USE_ALTERNATE_2
	struct bdev_handle *device_handle;
#endif /* VDO_USE_ALTERNATE_2 */
#else
	struct file *file_handle;
#endif /* VDO_USE_ALTERNATE */
};

void get_uds_io_factory(struct io_factory *factory)
{
	atomic_inc(&factory->ref_count);
}

static int get_block_device_from_name(const char *name,
				      struct io_factory *factory)
{
	dev_t device;
	unsigned int major, minor;
	char dummy;
#ifndef VDO_USE_ALTERNATE_3
	const struct blk_holder_ops hops = { NULL };
#endif /* VDO_USE_ALTERNATE_3 */

	/* Extract the major/minor numbers */
	if (sscanf(name, "%u:%u%c", &major, &minor, &dummy) == 2) {
		device = MKDEV(major, minor);
		if (MAJOR(device) != major || MINOR(device) != minor) {
			factory->bdev = NULL;
			return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
						      "%s is not a valid block device",
						      name);
		}

#ifdef VDO_USE_ALTERNATE
#ifdef VDO_USE_ALTERNATE_2
#ifdef VDO_USE_ALTERNATE_3
		factory->bdev = blkdev_get_by_dev(device, BLK_FMODE, NULL);
#else
		factory->bdev = blkdev_get_by_dev(device, BLK_FMODE, NULL, &hops);
#endif /* VDO_USE_ALTERNATE_3 */
#else
		factory->device_handle = bdev_open_by_dev(device, BLK_FMODE, NULL, &hops);
		if (IS_ERR(factory->device_handle)) {
			uds_log_error_strerror(-PTR_ERR(factory->device_handle),
					       "cannot get block device handle for %s", name);
			factory->device_handle = NULL;
			factory->bdev = NULL;
			return UDS_INVALID_ARGUMENT;
		}
		
		factory->bdev = factory->device_handle->bdev;
#endif /* VDO_USE_ALTERNATE_2 */
#else
		factory->file_handle = bdev_file_open_by_dev(device, BLK_FMODE, NULL, &hops);
		if (IS_ERR(factory->file_handle)) {
			uds_log_error_strerror(-PTR_ERR(factory->file_handle),
					       "cannot get file handle for %s", name);
			factory->file_handle = NULL;
			factory->bdev = NULL;
			return UDS_INVALID_ARGUMENT;
		}
		
		factory->bdev = file_bdev(factory->file_handle);
#endif /* VDO_USE_ALTERNATE */
	} else {
#ifdef VDO_USE_ALTERNATE
#ifdef VDO_USE_ALTERNATE_2
#ifdef VDO_USE_ALTERNATE_3
		factory->bdev = blkdev_get_by_path(name, BLK_FMODE, NULL);
#else
		factory->bdev = blkdev_get_by_path(name, BLK_FMODE, NULL, &hops);
#endif /* VDO_USE_ALTERNATE_3 */
#else
		factory->device_handle = bdev_open_by_path(name, BLK_FMODE, NULL, &hops);
		if (IS_ERR(factory->device_handle)) {
			uds_log_error_strerror(-PTR_ERR(factory->device_handle),
					       "cannot get block device handle for %s", name);
			factory->device_handle = NULL;
			factory->bdev = NULL;
			return UDS_INVALID_ARGUMENT;
		}
		
		factory->bdev = factory->device_handle->bdev;
#endif /* VDO_USE_ALTERNATE_2 */
#else
		factory->file_handle = bdev_file_open_by_path(name, BLK_FMODE, NULL, &hops);
		if (IS_ERR(factory->file_handle)) {
			uds_log_error_strerror(-PTR_ERR(factory->file_handle),
					       "cannot get file handle for %s", name);
			factory->file_handle = NULL;
			factory->bdev = NULL;
			return UDS_INVALID_ARGUMENT;
		}
		
		factory->bdev = file_bdev(factory->file_handle);
#endif /* VDO_USE_ALTERNATE */
	}

	if (IS_ERR(factory->bdev)) {
		uds_log_error_strerror(-PTR_ERR(factory->bdev),
				       "%s is not a block device", name);
		factory->bdev = NULL;
		return UDS_INVALID_ARGUMENT;
	}

	return UDS_SUCCESS;
}

int make_uds_io_factory(const char *path, struct io_factory **factory_ptr)
{
	int result;
	struct io_factory *factory;

	result = UDS_ALLOCATE(1, struct io_factory, __func__, &factory);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_block_device_from_name(path, factory);
	if (result != UDS_SUCCESS) {
		put_uds_io_factory(factory);
		return result;
	}

	atomic_set_release(&factory->ref_count, 1);

	*factory_ptr = factory;
	return UDS_SUCCESS;
}

int replace_uds_storage(struct io_factory *factory, const char *path)
{
	int result;
	struct io_factory new_factory;

	result = get_block_device_from_name(path, &new_factory);
	if (result != UDS_SUCCESS) {
		return result;
	}

#ifdef VDO_USE_ALTERNATE
#ifdef VDO_USE_ALTERNATE_2
#ifdef VDO_USE_ALTERNATE_3
	blkdev_put(factory->bdev, BLK_FMODE);
#else
	blkdev_put(factory->bdev, NULL);
#endif /* VDO_USE_ALTERNATE_3 */
#else
	bdev_release(factory->device_handle);
	factory->device_handle = new_factory.device_handle;
#endif /* VDO_USE_ALTERNATE_2 */
#else
	fput(factory->file_handle);
	factory->file_handle = new_factory.file_handle;
#endif /* VDO_USE_ALTERNATE */
	factory->bdev = new_factory.bdev;
	return UDS_SUCCESS;
}

void put_uds_io_factory(struct io_factory *factory)
{
	if (atomic_add_return(-1, &factory->ref_count) <= 0) {
#ifdef VDO_USE_ALTERNATE
#ifdef VDO_USE_ALTERNATE_2
#ifdef VDO_USE_ALTERNATE_3
		blkdev_put(factory->bdev, BLK_FMODE);
#else
		blkdev_put(factory->bdev, NULL);
#endif /* VDO_USE_ALTERNATE_3 */
#else
		bdev_release(factory->device_handle);
#endif /* VDO_USE_ALTERNATE_2 */
#else
		fput(factory->file_handle);
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
