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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/bio.c#21 $
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
void set_bio_operation(struct bio *bio, unsigned int operation)
{
	bio->bi_opf &= ~REQ_OP_MASK;
	bio->bi_opf |= operation;
}

/**********************************************************************/
void free_bio(struct bio *bio, struct kernel_layer *layer)
{
	bio_uninit(bio);
	FREE(bio);
}

/**********************************************************************/
void count_bios(struct atomic_bio_stats *bio_stats, struct bio *bio)
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
static void set_bio_size(struct bio *bio, block_size_t bio_size)
{
	bio->bi_iter.bi_size = bio_size;
}

/**
 * Initialize a bio.
 *
 * @param bio    The bio to initialize
 * @param layer  The layer to which it belongs.
 **/
static void initialize_bio(struct bio *bio, struct kernel_layer *layer)
{
	// Save off important info so it can be set back later
	unsigned short vcnt = bio->bi_vcnt;
	void *pvt = bio->bi_private;

	bio_reset(bio); // Memsets large portion of bio. Reset all needed
			// fields.
	bio->bi_private = pvt;
	bio->bi_vcnt = vcnt;
	bio->bi_end_io = complete_async_bio;
	set_bio_sector(bio, (sector_t) -1); // Sector will be set later on.
	set_bio_block_device(bio, get_kernel_layer_bdev(layer));
}

/**********************************************************************/
void reset_bio(struct bio *bio, struct kernel_layer *layer)
{
	// VDO-allocated bios always have a vcnt of 0 (for flushes) or 1 (for
	// data). Assert that this function is called on bios with vcnt of 0
	// or 1.
	ASSERT_LOG_ONLY((bio->bi_vcnt == 0) || (bio->bi_vcnt == 1),
			"initialize_bio only called on VDO-allocated bios");

	initialize_bio(bio, layer);

	// All VDO bios which are reset are expected to have their data, so
	// if they have a vcnt of 0, make it 1.
	if (bio->bi_vcnt == 0) {
		bio->bi_vcnt = 1;
	}

	set_bio_size(bio, VDO_BLOCK_SIZE);
}

/**********************************************************************/
int create_bio(struct kernel_layer *layer, char *data, struct bio **bio_ptr)
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

	initialize_bio(bio, layer);
	if (data == NULL) {
		*bio_ptr = bio;
		return VDO_SUCCESS;
	}

	int len = VDO_BLOCK_SIZE;
	int offset = offset_in_page(data);
	unsigned int i;

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
			free_bio(bio, layer);
			return logErrorWithStringError(
				VDO_BIO_CREATION_FAILED,
				"Could only add %i bytes to bio",
				bytes_added);
		}

		data += bytes;
		len -= bytes;
		offset = 0;
	}

	*bio_ptr = bio;
	return VDO_SUCCESS;
}

/**********************************************************************/
void prepare_flush_bio(struct bio *bio,
		       void *context,
		       struct block_device *device,
		       bio_end_io_t *end_io_callback)
{
	clear_bio_operation_and_flags(bio);
	/*
	 * One would think we could use REQ_OP_FLUSH on new kernels, but some
	 * layers of the stack don't recognize that as a flush. So do it
	 * like blkdev_issue_flush() and make it a write+flush.
	 */
	set_bio_operation_write(bio);
	set_bio_operation_flag_preflush(bio);
	bio->bi_end_io = end_io_callback;
	bio->bi_private = context;
	bio->bi_vcnt = 0;
	set_bio_block_device(bio, device);
	set_bio_size(bio, 0);
	set_bio_sector(bio, 0);
}
