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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/kernel/bio.h#1 $
 */

#ifndef BIO_H
#define BIO_H

#include <linux/bio.h>
#include <linux/blkdev.h>

#include "kernelTypes.h"

/**
 * Copy the bio data to a char array.
 *
 * @param bio       The bio to copy the data from
 * @param data_ptr  The local array to copy the data to
 **/
void vdo_bio_copy_data_in(struct bio *bio, char *data_ptr);

/**
 * Copy a char array to the bio data.
 *
 * @param bio       The bio to copy the data to
 * @param data_ptr  The local array to copy the data from
 **/
void vdo_bio_copy_data_out(struct bio *bio, char *data_ptr);

/**
 * Get the error from the bio.
 *
 * @param bio  The bio
 *
 * @return the bio's error if any
 **/
static inline int vdo_get_bio_result(struct bio *bio)
{
	return blk_status_to_errno(bio->bi_status);
}

/**
 * Tell the kernel we've completed processing of this bio.
 *
 * @param bio    The bio to complete
 * @param error  A system error code, or 0 for success
 **/
static inline void vdo_complete_bio(struct bio *bio, int error)
{
	bio->bi_status = errno_to_blk_status(error);
	bio_endio(bio);
}

/**
 * Frees up a bio structure
 *
 * @param bio    The bio to free
 **/
void vdo_free_bio(struct bio *bio);

/**
 * Count the statistics for the bios.  This is used for calls into VDO and
 * for calls out of VDO.
 *
 * @param bio_stats  Statistics structure to update
 * @param bio        The bio
 **/
void vdo_count_bios(struct atomic_bio_stats *bio_stats, struct bio *bio);

/**
 * Does all the appropriate accounting for bio completions
 *
 * @param bio  the bio to count
 **/
void vdo_count_completed_bios(struct bio *bio);

/**
 * Completes a bio relating to a vio, causing the completion callback to be
 * invoked.
 *
 * This is used as the bi_end_io function for most of the bios created within
 * VDO and submitted to the storage device. Exceptions are the flush code and
 * the read-block code, both of which need to regain control in the kernel
 * layer after the I/O is completed.
 *
 * @param bio   The bio to complete
 **/
void vdo_complete_async_bio(struct bio *bio);

/**
 * Reset a bio wholly, preparing it to perform an IO. May only be used on a
 * VDO-allocated bio, as it assumes the bio wraps a 4k buffer that is 4k
 * aligned.
 *
 * @param bio       The bio to reset
 * @param data      The data the bio should wrap
 * @param vio       The vio to which this bio belongs (may be NULL)
 * @param callback  The callback the bio should call when IO finishes
 * @param bi_opf    The operation and flags for the bio
 * @param pbn       The physical block number to write to
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_reset_bio_with_buffer(struct bio *bio,
			      char *data,
			      struct vio *vio,
			      bio_end_io_t callback,
			      unsigned int bi_opf,
			      physical_block_number_t pbn);

/**
 * Clone a user bio, then edit our copy to fit our needs.
 *
 * @param bio       The bio to reset
 * @param user_bio  The user bio to clone
 * @param vio       The vio to which our bio belongs (may be NULL)
 * @param callback  The callback our bio should call when IO finishes
 * @param bi_opf    The operation and flags for our bio
 * @param pbn       The physical block number to write to
 **/
void vdo_reset_bio_with_user_bio(struct bio *bio,
				 struct bio *user_bio,
				 struct vio *vio,
				 bio_end_io_t callback,
				 unsigned int bi_opf,
				 physical_block_number_t pbn);

/**
 * Create a new bio structure, which is guaranteed to be able to wrap any
 * contiguous buffer for IO.
 *
 * @param [out] bio_ptr  A pointer to hold new bio
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_create_bio(struct bio **bio_ptr);

#endif /* BIO_H */
