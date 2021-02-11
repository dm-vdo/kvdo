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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bio.c#47 $
 */

#include "bio.h"

#include <linux/version.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "kernelLayer.h"
#include "kvio.h"

enum { INLINE_BVEC_COUNT = 2 };

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

/**
 * Increments appropriate counters for bio completions
 *
 * @param vio  the vio associated with the bio
 * @param bio  the bio to count
 **/
static void count_all_bios_completed(struct vio *vio, struct bio *bio)
{
	struct kernel_layer *layer
		= as_kernel_layer(vio_as_completion(vio)->layer);
	if (is_data_vio(vio)) {
		count_bios(&layer->bios_out_completed, bio);
		return;
	}

	count_bios(&layer->bios_meta_completed, bio);
	if (vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
		count_bios(&layer->bios_journal_completed, bio);
	} else if (vio->type == VIO_TYPE_BLOCK_MAP) {
		count_bios(&layer->bios_page_cache_completed, bio);
	}
}

/**********************************************************************/
void count_completed_bios(struct bio *bio)
{
	struct vio *vio = (struct vio *) bio->bi_private;
	struct kernel_layer *layer
		= as_kernel_layer(vio_as_completion(vio)->layer);
	atomic64_inc(&layer->bios_completed);
	count_all_bios_completed(vio, bio);
}

/**********************************************************************/
void complete_async_bio(struct bio *bio)
{
	struct vio *vio = (struct vio *) bio->bi_private;
	count_completed_bios(bio);
	continue_vio(vio, get_bio_result(bio));
}

/**********************************************************************/
int reset_bio_with_buffer(struct bio *bio,
			  char *data,
			  struct vio *vio,
			  bio_end_io_t callback,
			  unsigned int bi_opf,
			  physical_block_number_t pbn)
{
	int bvec_count, result;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,1,0)
	struct page *page;
	int bytes_added;
#else
	int len = VDO_BLOCK_SIZE;
	int offset = offset_in_page(data);
	unsigned int i;
#endif // >= 5.1.0

	bio_reset(bio); // Memsets most of the bio to reset most fields.
	bio->bi_private = vio;
	bio->bi_end_io = callback;
	bio->bi_opf = bi_opf;
	bio->bi_iter.bi_sector = block_to_sector(pbn);
	if (data == NULL) {
		return VDO_SUCCESS;
	}

	// Make sure we use our own inlined iovecs.
	bio->bi_io_vec = bio->bi_inline_vecs;
	bio->bi_max_vecs = INLINE_BVEC_COUNT;

	bvec_count = (offset_in_page(data) + VDO_BLOCK_SIZE +
		      PAGE_SIZE - 1) >> PAGE_SHIFT;
	result = ASSERT(bvec_count <= INLINE_BVEC_COUNT,
			"VDO-allocated buffers lie on max %d pages, not %d",
			INLINE_BVEC_COUNT, bvec_count);
	if (result != UDS_SUCCESS) {
		return result;
	}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,1,0)
	// bio_add_page() can take any contiguous buffer on any number of
	// pages and add it in one shot.
	page = is_vmalloc_addr(data) ?  vmalloc_to_page(data) :
				        virt_to_page(data);
	bytes_added = bio_add_page(bio, page, VDO_BLOCK_SIZE,
				   offset_in_page(data));

	if (bytes_added != VDO_BLOCK_SIZE) {
		free_bio(bio);
		return log_error_strerror(VDO_BIO_CREATION_FAILED,
					  "Could only add %i bytes to bio",
					  bytes_added);
	}
#else
	// On pre-5.1 kernels, we have to add one page at a time to the bio.
	for (i = 0; (i < bvec_count) && (len > 0); i++) {
		unsigned int bytes = PAGE_SIZE - offset;
		struct page *page;
		int bytes_added;

		if (bytes > len) {
			bytes = len;
		}

		page = is_vmalloc_addr(data) ?  vmalloc_to_page(data) :
						virt_to_page(data);
		bytes_added = bio_add_page(bio, page, bytes, offset);

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
int create_bio(struct bio **bio_ptr)
{
	struct bio *bio = NULL;
	int result = ALLOCATE_EXTENDED(struct bio, INLINE_BVEC_COUNT,
				       struct bio_vec, "bio", &bio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*bio_ptr = bio;
	return VDO_SUCCESS;
}


