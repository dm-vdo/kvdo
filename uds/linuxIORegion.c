/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/gloria/kernelLinux/uds/linuxIORegion.c#1 $
 */

#include "linuxIORegion.h"

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/version.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "timeUtils.h"

// Size a bio to be able to read or write 64K in a single request.
enum { BVEC_COUNT = 64 * 1024 / PAGE_SIZE };

enum { BLK_FMODE = FMODE_READ | FMODE_WRITE };

typedef struct {
  struct completion *wait;
  int                result;
} LinuxIOCompletion;

enum { SECTOR_SHIFT = 9 };
enum { SECTOR_SIZE  = 1 << SECTOR_SHIFT };

typedef struct {
  IORegion             common;
  struct block_device *bdev;
  uint64_t             size;
  void                *zeroBlock;
} LinuxIORegion;

/*****************************************************************************/
static INLINE LinuxIORegion *asLinuxIORegion(IORegion *region)
{
  return container_of(region, LinuxIORegion, common);
}

/*****************************************************************************/
static int lior_close(IORegion *region)
{
  LinuxIORegion *lior = asLinuxIORegion(region);
  blkdev_put(lior->bdev, BLK_FMODE);
  FREE(lior->zeroBlock);
  FREE(lior);
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int validateIO(LinuxIORegion *lior,
                      off_t          offset,
                      void          *buffer,
                      size_t         size,
                      size_t        *length,
                      bool           writing)
{
  if (offset_in_page(buffer) != 0) {
    // No need to cry wolf.  Our callers are prepared to deal with this.
    return UDS_INCORRECT_ALIGNMENT;
  }

  if (offset % SECTOR_SIZE != 0) {
    return logErrorWithStringError(UDS_INCORRECT_ALIGNMENT,
                                   "disk alignment %zd not multiple of %d",
                                   offset, SECTOR_SIZE);
  }

  if (size % SECTOR_SIZE != 0) {
    return logErrorWithStringError(UDS_BUFFER_ERROR,
                                   "buffer size %zd not a multiple of %d",
                                   size, SECTOR_SIZE);
  }

  if (*length > size) {
    return logErrorWithStringError(UDS_BUFFER_ERROR,
                                   "length %zd exceeds buffer size %zd",
                                   *length, size);
  }

  if (offset > (off_t) lior->size) {
    return logErrorWithStringError(UDS_OUT_OF_RANGE,
                                   "offset %zd exceeds limit of %" PRId64,
                                   offset, lior->size);
  }

  if ((offset == (off_t) lior->size) && (*length > 0)) {
    return UDS_END_OF_FILE;
  }

  if (!writing) {
    *length = minSizeT(lior->size, offset + size) - offset;
  }
  if ((offset < 0) || ((offset + *length) > lior->size)) {
    return logErrorWithStringError(UDS_OUT_OF_RANGE,
                                   "range %zd-%zd not in range 0 to %" PRIu64,
                                   offset, offset + *length, lior->size);
  }

  return UDS_SUCCESS;
}

/*****************************************************************************/
static int lior_getLimit(IORegion *region, off_t *limit)
{
  LinuxIORegion *lior = asLinuxIORegion(region);
  *limit = lior->size;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int lior_getDataSize(IORegion *region, off_t *extent)
{
  LinuxIORegion *lior = asLinuxIORegion(region);
  *extent = lior->size;
  return UDS_SUCCESS;
}

/*****************************************************************************/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
static void lior_endio(struct bio *bio)
#else
static void lior_endio(struct bio *bio, int err)
#endif
{
  LinuxIOCompletion *lioc = (LinuxIOCompletion *)bio->bi_private;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
  lioc->result = blk_status_to_errno(bio->bi_status);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  lioc->result = -bio->bi_error;
#else
  lioc->result = -err;
#endif
  complete(lioc->wait);
}

/*****************************************************************************/
static void lior_bio_init(struct bio *bio, LinuxIORegion *lior, off_t offset)
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(3,13,0)
  bio_reset(bio);
  bio->bi_iter.bi_sector = offset >> SECTOR_SHIFT;
#else
  bio->bi_idx    = 0;
  bio->bi_next   = NULL;
  bio->bi_sector = offset >> SECTOR_SHIFT;
  bio->bi_size   = 0;
  bio->bi_vcnt   = 0;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
  bio_set_dev(bio, lior->bdev);
#else
  bio->bi_bdev = lior->bdev;
#endif
}

/*****************************************************************************/
static int lior_bio_submit(struct bio *bio, int rw)
{
  // XXX When we ditch Squeeze, we can use submit_bio_wait() instead of this pile.
  DECLARE_COMPLETION_ONSTACK(wait);
  init_completion(&wait);
  LinuxIOCompletion lioc = { .result = UDS_SUCCESS, .wait = &wait };
  bio->bi_end_io  = lior_endio;
  bio->bi_private = &lioc;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
  bio->bi_opf = rw;
  submit_bio(bio);
#else
  submit_bio(rw, bio);
#endif
  wait_for_completion(&wait);
  return lioc.result;
}

/*****************************************************************************/
static int lior_io(LinuxIORegion *lior,
                   off_t          offset,
                   byte          *buffer,
                   size_t         size,
                   int            rw)
{
  struct bio *bio = bio_alloc(GFP_KERNEL, BVEC_COUNT);
  if (IS_ERR(bio)) {
    return logErrorWithStringError(ENOMEM, "cannot allocate a struct bio");
  }
  lior_bio_init(bio, lior, offset);

  int result = UDS_SUCCESS;
  while ((result == UDS_SUCCESS) && (size > 0)) {
    unsigned int ioSize = size > PAGE_SIZE ? PAGE_SIZE : size;
    struct page *page = (is_vmalloc_addr(buffer)
                         ? vmalloc_to_page(buffer)
                         : virt_to_page(buffer));
    if (bio_add_page(bio, page, ioSize, 0) == 0) {
      // bio is full, so submit it ...
      result = lior_bio_submit(bio, rw);
      // ... and start a new bio
      lior_bio_init(bio, lior, offset);
    } else {
      buffer += ioSize;
      size   -= ioSize;
      offset += ioSize;
    }
  }
  if (result == UDS_SUCCESS) {
    // bio contains the last part of the buffer
    result = lior_bio_submit(bio, rw);
  }
  bio_put(bio);
  return result;
}

/*****************************************************************************/
static int lior_write(IORegion   *region,
                      off_t       offset,
                      const void *data,
                      size_t      size,
                      size_t      length)
{
  union { const void *data; void *buffer; } deconst;
  deconst.data = data;
  void *buffer = deconst.buffer;
  LinuxIORegion *lior = asLinuxIORegion(region);
  int result = validateIO(lior, offset, buffer, size, &length, true);
  if (result != UDS_SUCCESS) {
    return result;
  }
  task_io_account_write(size);
  return lior_io(lior, offset, buffer, size, WRITE);
}

/*****************************************************************************/
static int lior_clear(IORegion *region)
{
  // unlike other implementations we only clear the first block of the region,
  // because otherwise this could get very expensive

  LinuxIORegion *lior = asLinuxIORegion(region);
  return lior_write(region, 0, lior->zeroBlock, SECTOR_SIZE, SECTOR_SIZE);
}

/*****************************************************************************/
static int lior_read(IORegion *region,
                     off_t     offset,
                     void     *buffer,
                     size_t    size,
                     size_t   *length)
{
  LinuxIORegion *lior = asLinuxIORegion(region);
  size_t len = (length == NULL) ? size : *length;
  int result = validateIO(lior, offset, buffer, size, &len, false);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = lior_io(lior, offset, buffer, size, READ);

  if ((length != NULL) && (result == UDS_SUCCESS)) {
    *length = size;
  }
  return result;
}

/*****************************************************************************/
static int lior_getBlockSize(IORegion *region, size_t *blockSize)
{
  *blockSize = SECTOR_SIZE;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int lior_getBestSize(IORegion *region, size_t *bufferSize)
{
  *bufferSize = PAGE_SIZE;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int lior_syncContents(IORegion *region __attribute__((unused)))
{
  return UDS_SUCCESS;
}

/*****************************************************************************/
int openLinuxRegion(const char *path, uint64_t size, IORegion **regionPtr)
{
  if (size % SECTOR_SIZE != 0) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "size not a multiple of blockSize");
  }

  struct block_device *bdev = blkdev_get_by_path(path, BLK_FMODE, NULL);
  if (IS_ERR(bdev)) {
    logErrorWithStringError(-PTR_ERR(bdev), "%s is not a block device", path);
    return UDS_INVALID_ARGUMENT;
  }

  LinuxIORegion *lior = NULL;
  int result = ALLOCATE(1, LinuxIORegion, "Linux IO region", &lior);
  if (result != UDS_SUCCESS) {
    blkdev_put(bdev, BLK_FMODE);
    return result;
  }

  byte *buf;
  result = ALLOCATE_IO_ALIGNED(PAGE_SIZE, byte, "Linux zero block", &buf);
  if (result != UDS_SUCCESS) {
    FREE(lior);
    blkdev_put(bdev, BLK_FMODE);
    return result;
  }
  memset(buf, 0, PAGE_SIZE);

  lior->common.clear        = lior_clear;
  lior->common.close        = lior_close;
  lior->common.getBestSize  = lior_getBestSize;
  lior->common.getBlockSize = lior_getBlockSize;
  lior->common.getDataSize  = lior_getDataSize;
  lior->common.getLimit     = lior_getLimit;
  lior->common.read         = lior_read;
  lior->common.syncContents = lior_syncContents;
  lior->common.write        = lior_write;
  lior->bdev      = bdev;
  lior->size      = size;
  lior->zeroBlock = buf;
  *regionPtr = &lior->common;
  return UDS_SUCCESS;
}
