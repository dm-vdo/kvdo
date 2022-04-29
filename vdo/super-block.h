/* SPDX-License-Identifier: GPL-2.0-only */
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

#ifndef SUPER_BLOCK_H
#define SUPER_BLOCK_H

#include "kernel-types.h"
#include "types.h"

struct vdo_super_block;

void vdo_free_super_block(struct vdo_super_block *super_block);

void vdo_save_super_block(struct vdo_super_block *super_block,
			  physical_block_number_t super_block_offset,
			  struct vdo_completion *parent);

void vdo_load_super_block(struct vdo *vdo,
			  struct vdo_completion *parent,
			  physical_block_number_t super_block_offset,
			  struct vdo_super_block **super_block_ptr);

struct super_block_codec * __must_check
vdo_get_super_block_codec(struct vdo_super_block *super_block);

#endif /* SUPER_BLOCK_H */
