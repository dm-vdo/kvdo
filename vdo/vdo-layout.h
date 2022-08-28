/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

/**
 * DOC: VDO Layout.
 *
 * vdo_layout is an object which manages the layout of a VDO. It wraps
 * fixed_layout, but includes the knowledge of exactly which partitions a VDO
 * is expected to have. Because of this knowledge, the vdo_layout validates
 * the fixed_layout encoded in the super block at load time, obviating the
 * need for subsequent error checking when other modules need to get
 * partitions from the layout.
 *
 * The vdo_layout also manages the preparation and growth of the layout for
 * grow physical operations.
 */

#ifndef VDO_LAYOUT_H
#define VDO_LAYOUT_H

#include "buffer.h"

#include "kernel-types.h"

#include "types.h"

enum partition_direction {
	VDO_PARTITION_FROM_BEGINNING,
	VDO_PARTITION_FROM_END,
};

extern const block_count_t VDO_ALL_FREE_BLOCKS;

/*
 * A fixed layout is like a traditional disk partitioning scheme.  In the
 * beginning there is one large unused area, of which parts are carved off.
 * Each carved off section has its own internal offset and size.
 */
struct fixed_layout;
struct partition;

int __must_check vdo_make_fixed_layout(block_count_t total_blocks,
				       physical_block_number_t start_offset,
				       struct fixed_layout **layout_ptr);

void vdo_free_fixed_layout(struct fixed_layout *layout);

block_count_t __must_check
vdo_get_total_fixed_layout_size(const struct fixed_layout *layout);

int __must_check
vdo_get_fixed_layout_partition(struct fixed_layout *layout,
			       enum partition_id id,
			       struct partition **partition_ptr);

int __must_check
vdo_translate_to_pbn(const struct partition *partition,
		     physical_block_number_t partition_block_number,
		     physical_block_number_t *layer_block_number);

int __must_check
vdo_translate_from_pbn(const struct partition *partition,
		       physical_block_number_t layer_block_number,
		       physical_block_number_t *partition_block_number);

block_count_t __must_check
vdo_get_fixed_layout_blocks_available(const struct fixed_layout *layout);

int __must_check
vdo_make_fixed_layout_partition(struct fixed_layout *layout,
				enum partition_id id,
				block_count_t block_count,
				enum partition_direction direction,
				physical_block_number_t base);

block_count_t __must_check
vdo_get_fixed_layout_partition_size(const struct partition *partition);

physical_block_number_t __must_check
vdo_get_fixed_layout_partition_offset(const struct partition *partition);

physical_block_number_t __must_check
vdo_get_fixed_layout_partition_base(const struct partition *partition);

size_t __must_check
vdo_get_fixed_layout_encoded_size(const struct fixed_layout *layout);

int __must_check
vdo_encode_fixed_layout(const struct fixed_layout *layout, struct buffer *buffer);

int __must_check
vdo_decode_fixed_layout(struct buffer *buffer, struct fixed_layout **layout_ptr);

int __must_check
vdo_make_partitioned_fixed_layout(block_count_t physical_blocks,
				  physical_block_number_t starting_offset,
				  block_count_t block_map_blocks,
				  block_count_t journal_blocks,
				  block_count_t summary_blocks,
				  struct fixed_layout **layout_ptr);

/*-----------------------------------------------------------------*/

struct vdo_layout {
	/* The current layout of the VDO */
	struct fixed_layout *layout;
	/* The next layout of the VDO */
	struct fixed_layout *next_layout;
	/* The previous layout of the VDO */
	struct fixed_layout *previous_layout;
	/* The first block in the layouts */
	physical_block_number_t starting_offset;
	/* A pointer to the copy completion (if there is one) */
	struct dm_kcopyd_client *copier;
};

int __must_check vdo_decode_layout(struct fixed_layout *layout,
				   struct vdo_layout **vdo_layout_ptr);

void vdo_free_layout(struct vdo_layout *vdo_layout);

struct partition * __must_check
vdo_get_partition(struct vdo_layout *vdo_layout, enum partition_id id);

int __must_check
prepare_to_vdo_grow_layout(struct vdo_layout *vdo_layout,
			   block_count_t old_physical_blocks,
			   block_count_t new_physical_blocks);

block_count_t __must_check
vdo_get_next_layout_size(struct vdo_layout *vdo_layout);

block_count_t __must_check
vdo_get_next_block_allocator_partition_size(struct vdo_layout *vdo_layout);

block_count_t __must_check vdo_grow_layout(struct vdo_layout *vdo_layout);

void vdo_finish_layout_growth(struct vdo_layout *vdo_layout);

void vdo_copy_layout_partition(struct vdo_layout *layout,
			       enum partition_id id,
			       struct vdo_completion *parent);

struct fixed_layout * __must_check
vdo_get_fixed_layout(const struct vdo_layout *vdo_layout);

#endif /* VDO_LAYOUT_H */
