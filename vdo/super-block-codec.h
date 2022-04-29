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

#ifndef SUPER_BLOCK_CODEC_H
#define SUPER_BLOCK_CODEC_H

#include "buffer.h"

#include "header.h"
#include "types.h"

/*
 * The machinery for encoding and decoding super blocks.
 */
struct super_block_codec {
	/** The buffer for encoding and decoding component data */
	struct buffer *component_buffer;
	/**
	 * A sector-sized buffer wrapping the first sector of
	 * encoded_super_block, for encoding and decoding the entire super
	 * block.
	 **/
	struct buffer *block_buffer;
	/** A 1-block buffer holding the encoded on-disk super block */
	byte *encoded_super_block;
};

int __must_check vdo_initialize_super_block_codec(struct super_block_codec *codec);

void vdo_destroy_super_block_codec(struct super_block_codec *codec);

int __must_check vdo_encode_super_block(struct super_block_codec *codec);

int __must_check vdo_decode_super_block(struct super_block_codec *codec);

#endif /* SUPER_BLOCK_CODEC_H */
