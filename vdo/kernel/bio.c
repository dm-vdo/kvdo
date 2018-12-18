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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bio.c#3 $
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
void bioCopyDataIn(BIO *bio, char *dataPtr)
{
  struct bio_vec *biovec;
  for (struct bio_iterator iter = createBioIterator(bio);
       (biovec = getNextBiovec(&iter)) != NULL;
       advanceBioIterator(&iter)) {
    memcpy(dataPtr, getBufferForBiovec(biovec), biovec->bv_len);
    dataPtr += biovec->bv_len;
  }
}

/**********************************************************************/
void bioCopyDataOut(BIO *bio, char *dataPtr)
{
  struct bio_vec *biovec;
  for (struct bio_iterator iter = createBioIterator(bio);
       (biovec = getNextBiovec(&iter)) != NULL;
       advanceBioIterator(&iter)) {
    memcpy(getBufferForBiovec(biovec), dataPtr, biovec->bv_len);
    flush_dcache_page(biovec->bv_page);
    dataPtr += biovec->bv_len;
  }
}

/**********************************************************************/
void setBioOperation(BIO *bio, unsigned int operation)
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
void freeBio(BIO *bio, KernelLayer *layer)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
  bio_uninit(bio);
#endif
  FREE(bio);
}

/**********************************************************************/
void countBios(AtomicBioStats *bioStats, BIO *bio)
{
  if (isWriteBio(bio)) {
    atomic64_inc(&bioStats->write);
  } else {
    atomic64_inc(&bioStats->read);
  }
  if (isDiscardBio(bio)) {
    atomic64_inc(&bioStats->discard);
  }
  if (isFlushBio(bio)) {
    atomic64_inc(&bioStats->flush);
  }
  if (isFUABio(bio)) {
    atomic64_inc(&bioStats->fua);
  }
}

/**********************************************************************/
void bioZeroData(BIO *bio)
{
  zero_fill_bio(bio);
}

/**********************************************************************/
static void setBioSize(BIO *bio, BlockSize bioSize)
{
#ifdef USE_BI_ITER
  bio->bi_iter.bi_size = bioSize;
#else
  bio->bi_size = bioSize;
#endif
}

/**
 * Initialize a bio.
 *
 * @param bio    The bio to initialize
 * @param layer  The layer to which it belongs.
 **/
static void initializeBio(BIO *bio, KernelLayer *layer)
{
  // Save off important info so it can be set back later
  unsigned short   vcnt = bio->bi_vcnt;
  void            *pvt  = bio->bi_private;
  bio_reset(bio);     // Memsets large portion of bio. Reset all needed fields.
  bio->bi_private       = pvt;
  bio->bi_vcnt          = vcnt;
  bio->bi_end_io        = completeAsyncBio;
  setBioSector(bio, (sector_t) -1);  // Sector will be set later on.
  setBioBlockDevice(bio, getKernelLayerBdev(layer));
}

/**********************************************************************/
void resetBio(BIO *bio, KernelLayer *layer)
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
int createBio(KernelLayer *layer, char *data, BIO **bioPtr)
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

  BIO *bio = NULL;
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
    *bioPtr = bio;
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
  int bytesAdded = bio_add_page(bio, page, bytes, offset);
  if (bytesAdded != bytes) {
    freeBio(bio, layer);
    return logErrorWithStringError(VDO_BIO_CREATION_FAILED,
                                   "Could only add %i bytes to bio",
                                   bytesAdded);

    }

    data   += bytes;
    len    -= bytes;
    offset  = 0;
  }

  *bioPtr = bio;
  return VDO_SUCCESS;
}

/**********************************************************************/
void prepareFlushBIO(BIO                 *bio,
                     void                *context,
                     struct block_device *device,
                     bio_end_io_t        *endIOCallback)
{
  clearBioOperationAndFlags(bio);
  /*
   * One would think we could use REQ_OP_FLUSH on new kernels, but some
   * layers of the stack don't recognize that as a flush. So do it
   * like blkdev_issue_flush() and make it a write+flush.
   */
  setBioOperationWrite(bio);
  setBioOperationFlagPreflush(bio);
  bio->bi_end_io  = endIOCallback;
  bio->bi_private = context;
  bio->bi_vcnt    = 0;
  setBioBlockDevice(bio, device);
  setBioSize(bio, 0);
  setBioSector(bio, 0);
}
