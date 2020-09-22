/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bio.h#19 $
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
void bio_copy_data_in(struct bio *bio, char *data_ptr);

/**
 * Copy a char array to the bio data.
 *
 * @param bio       The bio to copy the data to
 * @param data_ptr  The local array to copy the data from
 **/
void bio_copy_data_out(struct bio *bio, char *data_ptr);

/**
 * Get the error from the bio.
 *
 * @param bio  The bio
 *
 * @return the bio's error if any
 **/
static inline int get_bio_result(struct bio *bio)
{
	return blk_status_to_errno(bio->bi_status);
}

/**
 * Tell the kernel we've completed processing of this bio.
 *
 * @param bio    The bio to complete
 * @param error  A system error code, or 0 for success
 **/
static inline void complete_bio(struct bio *bio, int error)
{
	bio->bi_status = errno_to_blk_status(error);
	bio_endio(bio);
}

/**
 * Frees up a bio structure
 *
 * @param bio    The bio to free
 **/
void free_bio(struct bio *bio);

/**
 * Count the statistics for the bios.  This is used for calls into VDO and
 * for calls out of VDO.
 *
 * @param bio_stats  Statistics structure to update
 * @param bio        The bio
 **/
void count_bios(struct atomic_bio_stats *bio_stats, struct bio *bio);

/**
 * Reset a bio so it can be used again. May only be used on a VDO-allocated
 * bio, as it assumes the bio wraps a 4k buffer that is 4k aligned.
 *
 * @param bio    The bio to reset
 **/
void reset_bio(struct bio *bio);

/**
 * Create a new bio structure for kernel buffer storage.
 *
 * @param [in]  data     The buffer (can be NULL)
 * @param [out] bio_ptr  A pointer to hold new bio
 *
 * @return VDO_SUCCESS or an error
 **/
int create_bio(char *data, struct bio **bio_ptr);

#endif /* BIO_H */
