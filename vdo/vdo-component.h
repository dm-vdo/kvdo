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

#ifndef VDO_COMPONENT_H
#define VDO_COMPONENT_H

#include "buffer.h"

#include "types.h"
#include "vdo-state.h"

/**
 * The configuration of the VDO service.
 **/
struct vdo_config {
	block_count_t logical_blocks; ///< number of logical blocks
	block_count_t physical_blocks; ///< number of physical blocks
	block_count_t slab_size; ///< number of blocks in a slab
	block_count_t recovery_journal_size; ///< number of recovery journal blocks
	block_count_t slab_journal_blocks; ///< number of slab journal blocks
};

/**
 * This is the structure that captures the vdo fields saved as a super block
 * component.
 **/
struct vdo_component {
	enum vdo_state state;
	uint64_t complete_recoveries;
	uint64_t read_only_recoveries;
	struct vdo_config config;
	nonce_t nonce;
};

size_t __must_check get_vdo_component_encoded_size(void);

int __must_check
encode_vdo_component(struct vdo_component component, struct buffer *buffer);

int __must_check
decode_vdo_component(struct buffer *buffer, struct vdo_component *component);

int validate_vdo_config(const struct vdo_config *config,
			block_count_t physical_block_count,
			block_count_t logical_block_count);

#endif /* VDO_COMPONENT_H */
