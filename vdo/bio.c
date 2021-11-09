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

#include "bio.h"

#include <linux/version.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "atomic-stats.h"
#include "kvio.h"
#include "vdo.h"

enum { INLINE_BVEC_COUNT = 2 };

/**
 * Copy the bio data to a char array.
 *
 * @param bio       The bio to copy the data from
 * @param data_ptr  The local array to copy the data to
 **/
void vdo_bio_copy_data_in(struct bio *bio, char *data_ptr)
{
	struct bio_vec biovec;
	struct bvec_iter iter;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0)
	unsigned long flags;

	bio_for_each_segment(biovec, bio, iter) {
		void *from = bvec_kmap_irq(&biovec, &flags);

		memcpy(data_ptr, from, biovec.bv_len);
		data_ptr += biovec.bv_len;
		bvec_kunmap_irq(from, &flags);
	}
#else

	bio_for_each_segment(biovec, bio, iter) {
		memcpy_from_bvec(data_ptr, &biovec);
		data_ptr += biovec.bv_len;
	}
#endif
}

/**
 * Copy a char array to the bio data.
 *
 * @param bio       The bio to copy the data to
 * @param data_ptr  The local array to copy the data from
 **/
void vdo_bio_copy_data_out(struct bio *bio, char *data_ptr)
{
	struct bio_vec biovec;
	struct bvec_iter iter;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0)
	unsigned long flags;

	bio_for_each_segment(biovec, bio, iter) {
		void *dest = bvec_kmap_irq(&biovec, &flags);

		memcpy(dest, data_ptr, biovec.bv_len);
		data_ptr += biovec.bv_len;
		flush_dcache_page(biovec.bv_page);
		bvec_kunmap_irq(dest, &flags);
	}
#else

	bio_for_each_segment(biovec, bio, iter) {
		memcpy_to_bvec(&biovec, data_ptr);
		data_ptr += biovec.bv_len;
	}
#endif
}

/**
 * Frees up a bio structure
 *
 * @param bio    The bio to free
 **/
void vdo_free_bio(struct bio *bio)
{
	if (bio == NULL) {
		return;
	}

	bio_uninit(bio);
	UDS_FREE(UDS_FORGET(bio));
}

/**
 * Count the statistics for the bios.  This is used for calls into VDO and
 * for calls out of VDO.
 *
 * @param bio_stats  Statistics structure to update
 * @param bio        The bio
 **/
void vdo_count_bios(struct atomic_bio_stats *bio_stats, struct bio *bio)
{
	if (((bio->bi_opf & REQ_PREFLUSH) != 0) &&
	    (bio->bi_iter.bi_size == 0)) {
		atomic64_inc(&bio_stats->empty_flush);
		atomic64_inc(&bio_stats->flush);
		return;
	}

	switch (bio_op(bio)) {
	case REQ_OP_WRITE:
		atomic64_inc(&bio_stats->write);
		break;
	case REQ_OP_READ:
		atomic64_inc(&bio_stats->read);
		break;
	case REQ_OP_DISCARD:
		atomic64_inc(&bio_stats->discard);
		break;
		/*
		 * All other operations are filtered out in dmvdo.c, or 
		 * not created by VDO, so shouldn't exist. 
		 */
	default:
		ASSERT_LOG_ONLY(0, "Bio operation %d not a write, read, discard, or empty flush",
				bio_op(bio));
	}

	if ((bio->bi_opf & REQ_PREFLUSH) != 0) {
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
	struct atomic_statistics *stats = &vdo_get_from_vio(vio)->stats;

	if (is_data_vio(vio)) {
		vdo_count_bios(&stats->bios_out_completed, bio);
		return;
	}

	vdo_count_bios(&stats->bios_meta_completed, bio);
	if (vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
		vdo_count_bios(&stats->bios_journal_completed, bio);
	} else if (vio->type == VIO_TYPE_BLOCK_MAP) {
		vdo_count_bios(&stats->bios_page_cache_completed, bio);
	}
}

/**
 * Does all the appropriate accounting for bio completions
 *
 * @param bio  the bio to count
 **/
void vdo_count_completed_bios(struct bio *bio)
{
	struct vio *vio = (struct vio *) bio->bi_private;
	atomic64_inc(&vdo_get_from_vio(vio)->stats.bios_completed);
	count_all_bios_completed(vio, bio);
}

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
void vdo_complete_async_bio(struct bio *bio)
{
	struct vio *vio = (struct vio *) bio->bi_private;

	vdo_count_completed_bios(bio);
	continue_vio(vio, vdo_get_bio_result(bio));
}

/**
 * Set bio properties for a VDO read or write.
 *
 * @param bio       The bio to reset
 * @param vio       The vio to which the bio belongs (may be NULL)
 * @param callback  The callback the bio should call when IO finishes
 * @param bi_opf    The operation and flags for the bio
 * @param pbn       The physical block number to write to
 **/
void vdo_set_bio_properties(struct bio *bio,
			    struct vio *vio,
			    bio_end_io_t callback,
			    unsigned int bi_opf,
			    physical_block_number_t pbn)
{
	bio->bi_private = vio;
	bio->bi_end_io = callback;
	bio->bi_opf = bi_opf;
	if ((vio != NULL) && (pbn != VDO_GEOMETRY_BLOCK_LOCATION)) {
		pbn -= vdo_get_from_vio(vio)->geometry.bio_offset;
	}
	bio->bi_iter.bi_sector = pbn * VDO_SECTORS_PER_BLOCK;
}

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
			      physical_block_number_t pbn)
{
	int bvec_count, result;
	struct page *page;
	int bytes_added;

	bio_reset(bio); /* Memsets most of the bio to reset most fields. */
	vdo_set_bio_properties(bio, vio, callback, bi_opf, pbn);
	if (data == NULL) {
		return VDO_SUCCESS;
	}

	/* Make sure we use our own inlined iovecs. */
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

	/*
	 * bio_add_page() can take any contiguous buffer on any number of 
	 * pages and add it in one shot. 
	 */
	page = is_vmalloc_addr(data) ? vmalloc_to_page(data) :
				       virt_to_page(data);
	bytes_added = bio_add_page(bio, page, VDO_BLOCK_SIZE,
				   offset_in_page(data));

	if (bytes_added != VDO_BLOCK_SIZE) {
		return uds_log_error_strerror(VDO_BIO_CREATION_FAILED,
					      "Could only add %i bytes to bio",
					      bytes_added);
	}

	return VDO_SUCCESS;
}

/**
 * Create a new bio structure, which is guaranteed to be able to wrap any
 * contiguous buffer for IO.
 *
 * @param [out] bio_ptr  A pointer to hold new bio
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_create_bio(struct bio **bio_ptr)
{
	struct bio *bio = NULL;
	int result = UDS_ALLOCATE_EXTENDED(struct bio, INLINE_BVEC_COUNT,
					   struct bio_vec, "bio", &bio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*bio_ptr = bio;
	return VDO_SUCCESS;
}

