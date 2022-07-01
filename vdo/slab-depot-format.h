/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SLAB_DEPOT_FORMAT_H
#define SLAB_DEPOT_FORMAT_H

#include "buffer.h"

#include "header.h"
#include "types.h"

struct slab_depot_state_2_0 {
	struct slab_config slab_config;
	physical_block_number_t first_block;
	physical_block_number_t last_block;
	zone_count_t zone_count;
} __packed;

extern const struct header VDO_SLAB_DEPOT_HEADER_2_0;

slab_count_t __must_check
vdo_compute_slab_count(physical_block_number_t first_block,
		       physical_block_number_t last_block,
		       unsigned int slab_size_shift);

size_t __must_check vdo_get_slab_depot_encoded_size(void);

int __must_check
vdo_encode_slab_depot_state_2_0(struct slab_depot_state_2_0 state,
				struct buffer *buffer);

int __must_check
vdo_decode_slab_depot_state_2_0(struct buffer *buffer,
				struct slab_depot_state_2_0 *state);

int __must_check vdo_configure_slab_depot(block_count_t block_count,
					  physical_block_number_t first_block,
					  struct slab_config slab_config,
					  zone_count_t zone_count,
					  struct slab_depot_state_2_0 *state);

int __must_check vdo_configure_slab(block_count_t slab_size,
				    block_count_t slab_journal_blocks,
				    struct slab_config *slab_config);

block_count_t __must_check
vdo_get_saved_reference_count_size(block_count_t block_count);

#endif /* SLAB_DEPOT_FORMAT_H */
