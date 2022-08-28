/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_COMPONENT_H
#define VDO_COMPONENT_H

#include "buffer.h"

#include "types.h"

/*
 * The configuration of the VDO service.
 */
struct vdo_config {
	block_count_t logical_blocks; /* number of logical blocks */
	block_count_t physical_blocks; /* number of physical blocks */
	block_count_t slab_size; /* number of blocks in a slab */
	block_count_t recovery_journal_size; /* number of recovery journal blocks */
	block_count_t slab_journal_blocks; /* number of slab journal blocks */
};

/*
 * This is the structure that captures the vdo fields saved as a super block
 * component.
 */
struct vdo_component {
	enum vdo_state state;
	uint64_t complete_recoveries;
	uint64_t read_only_recoveries;
	struct vdo_config config;
	nonce_t nonce;
};

size_t __must_check vdo_get_component_encoded_size(void);

int __must_check
vdo_encode_component(struct vdo_component component, struct buffer *buffer);

int __must_check
vdo_decode_component(struct buffer *buffer, struct vdo_component *component);

int vdo_validate_config(const struct vdo_config *config,
			block_count_t physical_block_count,
			block_count_t logical_block_count);

#endif /* VDO_COMPONENT_H */
