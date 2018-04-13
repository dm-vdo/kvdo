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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/bio.c#1 $
 */

#include "bio.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"

#include "flush.h"
#include "recoveryJournal.h"

#include "bioIterator.h"
#include "readCache.h"

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
  for (BioIterator iter = createBioIterator(bio);
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
  for (BioIterator iter = createBioIterator(bio);
       (biovec = getNextBiovec(&iter)) != NULL;
       advanceBioIterator(&iter)) {
    memcpy(getBufferForBiovec(biovec), dataPtr, biovec->bv_len);
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

#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  unsigned int OPERATION_MASK = WRITE | BIO_DISCARD | (1 << BIO_RW_FLUSH);
#else
  unsigned int OPERATION_MASK = WRITE | REQ_DISCARD | REQ_FLUSH;
#endif

  // Clear the relevant bits
  bio->bi_rw &= ~OPERATION_MASK;
  // Set the operation we care about
  bio->bi_rw |= operation;
#endif
}

/**********************************************************************/
void freeBio(BIO *bio, KernelLayer *layer)
{
  recordBioFree();
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0)
  bio_free(bio, layer->bioset);
#else
  bio_put(bio);
#endif
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

/**
 * The function determines whether a buffer contains all zeroes.
 *
 * @param buffer  The buffer to check
 * @param length  The length of the buffer
 *
 * @return true is all zeroes, false otherwise
 **/
static inline bool isAllZeros(const char *buffer, unsigned int length)
{
  /*
   * Handle expected common case of even the first word being nonzero,
   * without getting into the more expensive (for one iteration) loop
   * below.
   */
  if (likely(length >= sizeof(uint64_t))) {
    if (GET_UNALIGNED(uint64_t, buffer) != 0) {
      return false;
    }

    unsigned int wordCount = length / sizeof(uint64_t);

    // Unroll to process 64 bytes at a time
    unsigned int chunkCount = wordCount / 8;
    while (chunkCount-- > 0) {
      uint64_t word0 = GET_UNALIGNED(uint64_t, buffer);
      uint64_t word1 = GET_UNALIGNED(uint64_t, buffer + 1 * sizeof(uint64_t));
      uint64_t word2 = GET_UNALIGNED(uint64_t, buffer + 2 * sizeof(uint64_t));
      uint64_t word3 = GET_UNALIGNED(uint64_t, buffer + 3 * sizeof(uint64_t));
      uint64_t word4 = GET_UNALIGNED(uint64_t, buffer + 4 * sizeof(uint64_t));
      uint64_t word5 = GET_UNALIGNED(uint64_t, buffer + 5 * sizeof(uint64_t));
      uint64_t word6 = GET_UNALIGNED(uint64_t, buffer + 6 * sizeof(uint64_t));
      uint64_t word7 = GET_UNALIGNED(uint64_t, buffer + 7 * sizeof(uint64_t));
      uint64_t or = (word0 | word1 | word2 | word3
                     | word4 | word5 | word6 | word7);
      // Prevent compiler from using 8*(cmp;jne).
      __asm__ __volatile__ ("" : : "g" (or));
      if (or != 0) {
        return false;
      }
      buffer += 8 * sizeof(uint64_t);
    }
    wordCount %= 8;

    // Unroll to process 8 bytes at a time.
    // (Is this still worthwhile?)
    while (wordCount-- > 0) {
      if (GET_UNALIGNED(uint64_t, buffer) != 0) {
        return false;
      }
      buffer += sizeof(uint64_t);
    }
    length %= sizeof(uint64_t);
    // Fall through to finish up anything left over.
  }

  while (length-- > 0) {
    if (*buffer++ != 0) {
      return false;
    }
  }
  return true;
}

/**********************************************************************/
bool bioIsZeroData(BIO *bio)
{
  struct bio_vec *biovec;
  for (BioIterator iter = createBioIterator(bio);
       (biovec = getNextBiovec(&iter)) != NULL;
       advanceBioIterator(&iter)) {
    if (!isAllZeros(getBufferForBiovec(biovec), biovec->bv_len)) {
      return false;
    }
  }
  return true;
}

/**********************************************************************/
void bioZeroData(BIO *bio)
{
  /*
   * There's a routine zero_fill_bio exported from the kernel, but
   * this is a little faster.
   *
   * Taking apart what zero_fill_bio does: The HIGHMEM stuff isn't an
   * issue for x86_64, so bvec_k{,un}map_irq does no more than we do
   * here. On x86 flush_dcache_page doesn't do anything. And the
   * memset call there seems to be expanded inline by the compiler as
   * a "rep stosb" loop which is slower than the kernel-exported
   * memset.
   *
   * So we're functionally the same, and a little bit faster, this
   * way.
   */
  struct bio_vec *biovec;
  for (BioIterator iter = createBioIterator(bio);
       (biovec = getNextBiovec(&iter)) != NULL;
       advanceBioIterator(&iter)) {
    memset(getBufferForBiovec(biovec), 0, biovec->bv_len);
  }
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
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0)
  bio->bi_destructor = NULL;
  bio->bi_flags      = 1 << BIO_UPTODATE;
  bio->bi_idx        = 0;
  bio->bi_rw         = 0;
#else
  // Save off important info so it can be set back later
  unsigned short  vcnt = bio->bi_vcnt;
  void           *pvt  = bio->bi_private;
  bio_reset(bio);     // Memsets large portion of bio. Reset all needed fields.
  bio->bi_private      = pvt;
  bio->bi_vcnt         = vcnt;
#endif
  bio->bi_end_io     = completeAsyncBio;
  setBioSector(bio, (sector_t) -1);  // Sector will be set later on.
  setBioBlockDevice(bio, layer->dev->bdev);
}

/**********************************************************************/
void resetBio(BIO *bio, KernelLayer *layer)
{
  initializeBio(bio, layer);
  setBioSize(bio, VDO_BLOCK_SIZE);
}

/**********************************************************************/
int allocateBio(KernelLayer *layer, unsigned int bvecCount, BIO **bioPtr)
{
  BIO *bio = bio_alloc_bioset(GFP_NOIO, bvecCount, layer->bioset);
  if (IS_ERR(bio)) {
    logError("bio allocation failure %ld", PTR_ERR(bio));
    return PTR_ERR(bio);
  }
  recordBioAlloc();

  initializeBio(bio, layer);

  *bioPtr = bio;
  return VDO_SUCCESS;
}

/**********************************************************************/
int createBio(KernelLayer *layer, char *data, BIO **bioPtr)
{
  BIO *bio = NULL;
  if (data == NULL) {
    int result = allocateBio(layer, 0, &bio);
    if (result != VDO_SUCCESS) {
      return result;
    }

    *bioPtr = bio;
    return VDO_SUCCESS;
  }

  unsigned int  len       = VDO_BLOCK_SIZE;
  unsigned long kaddr     = (unsigned long) data;
  unsigned long end       = (kaddr + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
  unsigned long start     = kaddr >> PAGE_SHIFT;
  const int     bvecCount = end - start;

  int result = allocateBio(layer, bvecCount, &bio);
  if (result != VDO_SUCCESS) {
    return result;
  }

  int offset = offset_in_page(kaddr);
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
  setBioOperationFlush(bio);
  bio->bi_end_io  = endIOCallback;
  bio->bi_private = context;
  bio->bi_vcnt    = 0;
  setBioBlockDevice(bio, device);
  setBioSize(bio, 0);
  setBioSector(bio, 0);
}
