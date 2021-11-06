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
 */

#ifndef BIO_H
#define BIO_H

#include <linux/bio.h>
#include <linux/blkdev.h>

#include "kernel-types.h"

void vdo_bio_copy_data_in(struct bio *bio, char *data_ptr);

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

void vdo_free_bio(struct bio *bio);

void vdo_count_bios(struct atomic_bio_stats *bio_stats, struct bio *bio);

void vdo_count_completed_bios(struct bio *bio);

void vdo_complete_async_bio(struct bio *bio);

void vdo_set_bio_properties(struct bio *bio,
			    struct vio *vio,
			    bio_end_io_t callback,
			    unsigned int bi_opf,
			    physical_block_number_t pbn);

int vdo_reset_bio_with_buffer(struct bio *bio,
			      char *data,
			      struct vio *vio,
			      bio_end_io_t callback,
			      unsigned int bi_opf,
			      physical_block_number_t pbn);

int vdo_create_bio(struct bio **bio_ptr);

#endif /* BIO_H */
