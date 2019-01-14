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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bio.c#7 $
 */

#include "bio.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"

#include "flush.h"
#include "recoveryJournal.h"

#include "bioIterator.h"
#include "ioSubmitter.h"

/**
 * Gets the raw buffer from a biovec.
 *
 * @param biovec  The biovec in question
 *
 * @return the buffer
 **/
static char *getBufferForBiovec(struct bio_vec *biovec)
{
  return (page_address(biovec->bv_page) + biovec->bv_offset);
}

/**********************************************************************/
void bio_copy_data_in(struct bio *bio, char *data_ptr)
{
  struct bio_vec *biovec;
  for (struct bio_iterator iter = create_bio_iterator(bio);
       (biovec = get_next_biovec(&iter)) != NULL;
       advance_bio_iterator(&iter)) {
    memcpy(data_ptr, getBufferForBiovec(biovec), biovec->bv_len);
    data_ptr += biovec->bv_len;
  }
}

/**********************************************************************/
void bio_copy_data_out(struct bio *bio, char *data_ptr)
{
  struct bio_vec *biovec;
  for (struct bio_iterator iter = create_bio_iterator(bio);
       (biovec = get_next_biovec(&iter)) != NULL;
       advance_bio_iterator(&iter)) {
    memcpy(getBufferForBiovec(biovec), data_ptr, biovec->bv_len);
    flush_dcache_page(biovec->bv_page);
    data_ptr += biovec->bv_len;
  }
}

/**********************************************************************/
void set_bio_operation(struct bio *bio, unsigned int operation)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  bio->bi_opf &= ~REQ_OP_MASK;
  bio->bi_opf |= operation;
#else

  unsigned int OPERATION_MASK = WRITE | REQ_DISCARD | REQ_FLUSH;

  // Clear the relevant bits
  bio->bi_rw &= ~OPERATION_MASK;
  // Set the operation we care about
  bio->bi_rw |= operation;
#endif
}

/**********************************************************************/
void free_bio(struct bio *bio, KernelLayer *layer)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
  bio_uninit(bio);
#endif
  FREE(bio);
}

/**********************************************************************/
void count_bios(AtomicBioStats *bio_stats, struct bio *bio)
{
  if (is_write_bio(bio)) {
    atomic64_inc(&bio_stats->write);
  } else {
    atomic64_inc(&bio_stats->read);
  }
  if (is_discard_bio(bio)) {
    atomic64_inc(&bio_stats->discard);
  }
  if (is_flush_bio(bio)) {
    atomic64_inc(&bio_stats->flush);
  }
  if (is_fua_bio(bio)) {
    atomic64_inc(&bio_stats->fua);
  }
}

/**********************************************************************/
void bio_zero_data(struct bio *bio)
{
  zero_fill_bio(bio);
}

/**********************************************************************/
static void setBioSize(struct bio *bio, BlockSize bio_size)
{
#ifdef USE_BI_ITER
  bio->bi_iter.bi_size = bio_size;
#else
  bio->bi_size = bio_size;
#endif
}

/**
 * Initialize a bio.
 *
 * @param bio    The bio to initialize
 * @param layer  The layer to which it belongs.
 **/
static void initializeBio(struct bio *bio, KernelLayer *layer)
{
  // Save off important info so it can be set back later
  unsigned short   vcnt = bio->bi_vcnt;
  void            *pvt  = bio->bi_private;
  bio_reset(bio);     // Memsets large portion of bio. Reset all needed fields.
  bio->bi_private       = pvt;
  bio->bi_vcnt          = vcnt;
  bio->bi_end_io        = completeAsyncBio;
  set_bio_sector(bio, (sector_t) -1);  // Sector will be set later on.
  set_bio_block_device(bio, getKernelLayerBdev(layer));
}

/**********************************************************************/
void reset_bio(struct bio *bio, KernelLayer *layer)
{
  // VDO-allocated bios always have a vcnt of 0 (for flushes) or 1 (for data).
  // Assert that this function is called on bios with vcnt of 0 or 1.
  ASSERT_LOG_ONLY((bio->bi_vcnt == 0) || (bio->bi_vcnt == 1),
                  "initializeBio only called on VDO-allocated bios");

  initializeBio(bio, layer);

  // All VDO bios which are reset are expected to have their data, so
  // if they have a vcnt of 0, make it 1.
  if (bio->bi_vcnt == 0) {
    bio->bi_vcnt = 1;
  }

  setBioSize(bio, VDO_BLOCK_SIZE);
}

/**********************************************************************/
int create_bio(KernelLayer *layer, char *data, struct bio **bio_ptr)
{
  int bvecCount = 0;
  if (data != NULL) {
    bvecCount
      = (offset_in_page(data) + VDO_BLOCK_SIZE + PAGE_SIZE - 1) >> PAGE_SHIFT;
    /*
     * When restoring a bio after using it to flush, we don't know what data
     * it wraps so we just set the bvec count back to its original value.
     * This relies on the underlying storage not clearing bvecs that are not
     * in use. The original value also needs to be a constant, since we have
     * nowhere to store it during the time the bio is flushing.
     *
     * Fortunately our VDO-allocated bios always wrap exactly 4k, and the
     * allocator always gives us 4k-aligned buffers, and PAGE_SIZE is always
     * a multiple of 4k. So we only need one bvec to record the bio wrapping
     * a buffer of our own use, the original value is always 1, and this
     * assertion makes sure that stays true.
     */
    int result = ASSERT(bvecCount == 1,
                        "VDO-allocated buffers lie on 1 page, not %d",
                        bvecCount);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  struct bio *bio = NULL;
  int result = ALLOCATE_EXTENDED(struct bio, bvecCount, struct bio_vec,
                                 "bio", &bio);
  if (result != VDO_SUCCESS) {
    return result;
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  bio_init(bio, bio->bi_inline_vecs, bvecCount);
#else
  bio_init(bio);
  bio->bi_io_vec   = bio->bi_inline_vecs;
  bio->bi_max_vecs = bvecCount;
#endif

  initializeBio(bio, layer);
  if (data == NULL) {
    *bio_ptr = bio;
    return VDO_SUCCESS;
  }

  int len    = VDO_BLOCK_SIZE;
  int offset = offset_in_page(data);
  for (unsigned int i = 0; (i < bvecCount) && (len > 0); i++) {
    unsigned int bytes = PAGE_SIZE - offset;
    if (bytes > len) {
      bytes = len;
    }

  struct page *page
    = is_vmalloc_addr(data) ? vmalloc_to_page(data) : virt_to_page(data);
  int bytes_added = bio_add_page(bio, page, bytes, offset);
  if (bytes_added != bytes) {
    free_bio(bio, layer);
    return logErrorWithStringError(VDO_BIO_CREATION_FAILED,
                                   "Could only add %i bytes to bio",
                                   bytes_added);

    }

    data   += bytes;
    len    -= bytes;
    offset  = 0;
  }

  *bio_ptr = bio;
  return VDO_SUCCESS;
}

/**********************************************************************/
void prepare_flush_bio(struct bio          *bio,
                       void                *context,
                       struct block_device *device,
                       bio_end_io_t        *end_io_callback)
{
  clear_bio_operation_and_flags(bio);
  /*
   * One would think we could use REQ_OP_FLUSH on new kernels, but some
   * layers of the stack don't recognize that as a flush. So do it
   * like blkdev_issue_flush() and make it a write+flush.
   */
  set_bio_operation_write(bio);
  set_bio_operation_flag_preflush(bio);
  bio->bi_end_io  = end_io_callback;
  bio->bi_private = context;
  bio->bi_vcnt    = 0;
  set_bio_block_device(bio, device);
  setBioSize(bio, 0);
  set_bio_sector(bio, 0);
}
