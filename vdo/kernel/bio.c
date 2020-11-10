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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bio.c#36 $
 */

#include "bio.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"

#include "flush.h"
#include "recoveryJournal.h"

#include "ioSubmitter.h"

/**********************************************************************/
void bio_copy_data_in(struct bio *bio, char *data_ptr)
{
	struct bio_vec biovec;
	struct bvec_iter iter;
	unsigned long flags;

	bio_for_each_segment(biovec, bio, iter) {
		void *from = bvec_kmap_irq(&biovec, &flags);

		memcpy(data_ptr, from, biovec.bv_len);
		data_ptr += biovec.bv_len;
		bvec_kunmap_irq(from, &flags);
	}
}

/**********************************************************************/
void bio_copy_data_out(struct bio *bio, char *data_ptr)
{
	struct bio_vec biovec;
	struct bvec_iter iter;
	unsigned long flags;

	bio_for_each_segment(biovec, bio, iter) {
		void *dest = bvec_kmap_irq(&biovec, &flags);

		memcpy(dest, data_ptr, biovec.bv_len);
		data_ptr += biovec.bv_len;
		flush_dcache_page(biovec.bv_page);
		bvec_kunmap_irq(dest, &flags);
	}
}

/**********************************************************************/
void free_bio(struct bio *bio)
{
	bio_uninit(bio);
	FREE(bio);
}

/**********************************************************************/
void count_bios(struct atomic_bio_stats *bio_stats, struct bio *bio)
{
	if (bio_data_dir(bio) == WRITE) {
		atomic64_inc(&bio_stats->write);
	} else {
		atomic64_inc(&bio_stats->read);
	}
	if (bio_op(bio) == REQ_OP_DISCARD) {
		atomic64_inc(&bio_stats->discard);
	}
	if ((bio_op(bio) == REQ_OP_FLUSH) ||
	    ((bio->bi_opf & REQ_PREFLUSH) != 0)) {
		atomic64_inc(&bio_stats->flush);
	}
	if (bio->bi_opf & REQ_FUA) {
		atomic64_inc(&bio_stats->fua);
	}
}

/**********************************************************************/
int reset_bio_with_buffer(struct bio *bio,
			  char *data,
			  struct kvio *kvio,
			  bio_end_io_t callback,
			  unsigned int bi_opf,
			  physical_block_number_t pbn)
{
	bio_reset(bio); // Memsets most of the bio to reset most fields.
	bio->bi_private = kvio;
	bio->bi_end_io = callback;
	bio->bi_opf = bi_opf;
	bio->bi_iter.bi_sector = block_to_sector(pbn);
	if (data == NULL) {
		return VDO_SUCCESS;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,1,0)
	// bio_add_page() can take any contiguous buffer on any number of
	// pages and add it in one shot.
	struct page *page = is_vmalloc_addr(data) ?
				    vmalloc_to_page(data) :
				    virt_to_page(data);
	int bytes_added = bio_add_page(bio, page, VDO_BLOCK_SIZE,
				       offset_in_page(data));

	if (bytes_added != VDO_BLOCK_SIZE) {
		free_bio(bio);
		return log_error_strerror(VDO_BIO_CREATION_FAILED,
					  "Could only add %i bytes to bio",
					  bytes_added);
	}
#else
	// On pre-5.1 kernels, we have to add one page at a time to the bio.
	int len = VDO_BLOCK_SIZE;
	int offset = offset_in_page(data);
	unsigned int i;

	int bvec_count = (offset_in_page(data) + VDO_BLOCK_SIZE +
			  PAGE_SIZE - 1) >> PAGE_SHIFT;
	for (i = 0; (i < bvec_count) && (len > 0); i++) {
		unsigned int bytes = PAGE_SIZE - offset;

		if (bytes > len) {
			bytes = len;
		}

		struct page *page = is_vmalloc_addr(data) ?
					    vmalloc_to_page(data) :
					    virt_to_page(data);
		int bytes_added = bio_add_page(bio, page, bytes, offset);

		if (bytes_added != bytes) {
			free_bio(bio);
			return log_error_strerror(VDO_BIO_CREATION_FAILED,
						  "Could only add %i bytes to bio",
						  bytes_added);
		}

		data += bytes;
		len -= bytes;
		offset = 0;
	}
#endif // >= 5.1.0
	return VDO_SUCCESS;
}

/**********************************************************************/
int create_bio(char *data, struct bio **bio_ptr)
{
	int bvec_count = 0;

	if (data != NULL) {
		bvec_count = (offset_in_page(data) + VDO_BLOCK_SIZE +
			      PAGE_SIZE - 1) >> PAGE_SHIFT;
		/*
		 * When restoring a bio after using it to flush, we don't know
		 * what data it wraps so we just set the bvec count back to its
		 * original value. This relies on the underlying storage not
		 * clearing bvecs that are not in use. The original value also
		 * needs to be a constant, since we have nowhere to store it
		 * during the time the bio is flushing.
		 *
		 * Fortunately our VDO-allocated bios always wrap exactly 4k,
		 * and the allocator always gives us 4k-aligned buffers, and
		 * PAGE_SIZE is always a multiple of 4k. So we only need one
		 * bvec to record the bio wrapping a buffer of our own use, the
		 * original value is always 1, and this assertion makes sure
		 * that stays true.
		 */
		int result =
			ASSERT(bvec_count == 1,
			       "VDO-allocated buffers lie on 1 page, not %d",
			       bvec_count);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	struct bio *bio = NULL;
	int result = ALLOCATE_EXTENDED(struct bio, bvec_count, struct bio_vec,
				       "bio", &bio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	bio_init(bio, bio->bi_inline_vecs, bvec_count);

	result = reset_bio_with_buffer(bio, data, NULL, complete_async_bio, 0,
				       (sector_t) -1);
	if (result != VDO_SUCCESS) {
		free_bio(bio);
		return result;
	}

	*bio_ptr = bio;
	return VDO_SUCCESS;
}


