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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/bio.h#2 $
 */

#ifndef BIO_H
#define BIO_H

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/version.h>

#include "kernelTypes.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
#define USE_BI_ITER 1
#endif

/**
 * Copy the bio data to a char array.
 *
 * @param bio      The bio to copy the data from
 * @param dataPtr  The local array to copy the data to
 **/
void bioCopyDataIn(BIO *bio, char *dataPtr);

/**
 * Copy a char array to the bio data.
 *
 * @param bio      The bio to copy the data to
 * @param dataPtr  The local array to copy the data from
 **/
void bioCopyDataOut(BIO *bio, char *dataPtr);

/**
 * Set the bi_rw or equivalent field of a bio to a particular data
 * operation. Intended to be called only by setBioOperationRead() etc.
 *
 * @param bio        The bio to modify
 * @param operation  The operation to set it to
 **/
void setBioOperation(BIO *bio, unsigned int operation);

/**********************************************************************/
static inline void setBioOperationRead(BIO *bio)
{
  setBioOperation(bio, READ);
}

/**********************************************************************/
static inline void setBioOperationWrite(BIO *bio)
{
  setBioOperation(bio, WRITE);
}

/**********************************************************************/
static inline void setBioOperationFlush(BIO *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  setBioOperation(bio, REQ_OP_FLUSH);
#else
  setBioOperation(bio, WRITE_FLUSH);
#endif
}

/**********************************************************************/
static inline void clearBioOperationAndFlags(BIO *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  bio->bi_opf = 0;
#else
  bio->bi_rw = 0;
#endif
}

/**********************************************************************/
static inline void copyBioOperationAndFlags(BIO *to, BIO *from)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  to->bi_opf = from->bi_opf;
#else
  to->bi_rw = from->bi_rw;
#endif
}

/**********************************************************************/
static inline void setBioOperationFlag(BIO *bio, unsigned int flag)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  bio->bi_opf |= flag;
#else
  bio->bi_rw |= flag;
#endif
}

/**********************************************************************/
static inline void clearBioOperationFlag(BIO *bio, unsigned int flag)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  bio->bi_opf &= ~flag;
#else
  bio->bi_rw &= ~flag;
#endif
}

/**********************************************************************/
static inline void setBioOperationFlagPreflush(BIO *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  setBioOperationFlag(bio, REQ_PREFLUSH);
#else
  // Preflushes and empty flushes are not currently distinguished.
  setBioOperation(bio, WRITE_FLUSH);
#endif
}

/**********************************************************************/
static inline void setBioOperationFlagSync(BIO *bio)
{
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  setBioOperationFlag(bio, 1 << BIO_RW_SYNCIO);
#else
  setBioOperationFlag(bio, REQ_SYNC);
#endif
}

/**********************************************************************/
static inline void clearBioOperationFlagSync(BIO *bio)
{
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  clearBioOperationFlag(bio, 1 << BIO_RW_SYNCIO);
#else
  clearBioOperationFlag(bio, REQ_SYNC);
#endif
}

/**********************************************************************/
static inline void setBioOperationFlagFua(BIO *bio)
{
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  setBioOperationFlag(bio, BIO_FUA);
#else
  setBioOperationFlag(bio, REQ_FUA);
#endif
}

/**********************************************************************/
static inline void clearBioOperationFlagFua(BIO *bio)
{
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  clearBioOperationFlag(bio, BIO_FUA);
#else
  clearBioOperationFlag(bio, REQ_FUA);
#endif
}

/**********************************************************************/
static inline bool isDiscardBio(BIO *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  return (bio != NULL) && (bio_op(bio) == REQ_OP_DISCARD);
#elif LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  return (bio != NULL) && bio_rw_flagged(bio, BIO_RW_DISCARD);
#else
  return (bio != NULL) && ((bio->bi_rw & REQ_DISCARD) != 0);
#endif
}

/**********************************************************************/
static inline bool isFlushBio(BIO *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  return (bio_op(bio) == REQ_OP_FLUSH) || ((bio->bi_opf & REQ_PREFLUSH) != 0);
#elif LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  return bio_empty_barrier(bio) || bio_rw_flagged(bio, BIO_RW_FLUSH);
#else
  return (bio->bi_rw & REQ_FLUSH) != 0;
#endif
}

/**********************************************************************/
static inline bool isFUABio(BIO *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  return (bio->bi_opf & REQ_FUA) != 0;
#elif LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  return bio_rw_flagged(bio, BIO_RW_FUA);
#else
  return (bio->bi_rw & REQ_FUA) != 0;
#endif
}

/**********************************************************************/
static inline bool isReadBio(BIO *bio)
{
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  return !bio_rw_flagged(bio, BIO_RW);
#else
  return bio_data_dir(bio) == READ;
#endif
}

/**********************************************************************/
static inline bool isWriteBio(BIO *bio)
{
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32)
  return bio_rw_flagged(bio, BIO_RW);
#else
  return bio_data_dir(bio) == WRITE;
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Get the error from the bio.
 *
 * @param bio  The bio
 *
 * @return the bio's error if any
 **/
static inline int getBioResult(BIO *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
  return blk_status_to_errno(bio->bi_status);
#else
  return bio->bi_error;
#endif
}
#endif // newer than 4.4

/**
 * Set the block device for a bio.
 *
 * @param bio     The bio to modify
 * @param device  The new block device for the bio
 **/
static inline void setBioBlockDevice(BIO *bio, struct block_device *device)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
  bio_set_dev(bio, device);
#else
  bio->bi_bdev = device;
#endif
}

/**
 * Get a bio's size.
 *
 * @param bio  The bio
 *
 * @return the bio's size
 **/
static inline unsigned int getBioSize(BIO *bio)
{
#ifdef USE_BI_ITER
  return bio->bi_iter.bi_size;
#else
  return bio->bi_size;
#endif
}

/**
 * Set the bio's sector.
 *
 * @param bio     The bio
 * @param sector  The sector
 **/
static inline void setBioSector(BIO *bio, sector_t sector)
{
#ifdef USE_BI_ITER
  bio->bi_iter.bi_sector = sector;
#else
  bio->bi_sector = sector;
#endif
}

/**
 * Get the bio's sector.
 *
 * @param bio  The bio
 *
 * @return the sector
 **/
static inline sector_t getBioSector(BIO *bio)
{
#ifdef USE_BI_ITER
  return bio->bi_iter.bi_sector;
#else
  return bio->bi_sector;
#endif
}

/**
 * Tell the kernel we've completed processing of this bio.
 *
 * @param bio    The bio to complete
 * @param error  A system error code, or 0 for success
 **/
static inline void completeBio(BIO *bio, int error)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
  bio->bi_status = errno_to_blk_status(error);
  bio_endio(bio);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  bio->bi_error = error;
  bio_endio(bio);
#else
  bio_endio(bio, error);
#endif
}

/**
 * Frees up a bio structure
 *
 * @param bio    The bio to free
 * @param layer  The layer the bio was created in
 **/
void freeBio(BIO *bio, KernelLayer *layer);

/**
 * Count the statistics for the bios.  This is used for calls into VDO and
 * for calls out of VDO.
 *
 * @param bioStats  Statistics structure to update
 * @param bio       The bio
 **/
void countBios(AtomicBioStats *bioStats, BIO *bio);

/**
 * Reset a bio so it can be used again.
 *
 * @param bio    The bio to reset
 * @param layer  The physical layer
 **/
void resetBio(BIO *bio, KernelLayer *layer);

/**
 * Check to see whether a bio's data are all zeroes.
 *
 * @param bio  The bio
 *
 * @return true if the bio's data are all zeroes
 **/
bool bioIsZeroData(BIO *bio);

/**
 * Set a bio's data to all zeroes.
 *
 * @param [in] bio  The bio
 **/
void bioZeroData(BIO *bio);

/**
 * Create a new bio structure for kernel buffer storage.
 *
 * @param [in]  layer   The physical layer
 * @param [in]  data    The buffer (can be NULL)
 * @param [out] bioPtr  A pointer to hold new bio
 *
 * @return VDO_SUCCESS or an error
 **/
int createBio(KernelLayer *layer, char *data, BIO **bioPtr);

/**
 * Prepare a BIO to issue a flush to the device below.
 *
 * @param bio            The flush BIO
 * @param context        The context for the callback
 * @param device         The device to flush
 * @param endIOCallback  The function to call when the flush is complete
 **/
void prepareFlushBIO(BIO                 *bio,
                     void                *context,
                     struct block_device *device,
                     bio_end_io_t        *endIOCallback);

#endif /* BIO_H */
