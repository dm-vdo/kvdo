/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
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
	/* The buffer for encoding and decoding component data */
	struct buffer *component_buffer;
	/*
	 * A sector-sized buffer wrapping the first sector of
	 * encoded_super_block, for encoding and decoding the entire super
	 * block.
	 */
	struct buffer *block_buffer;
	/* A 1-block buffer holding the encoded on-disk super block */
	byte *encoded_super_block;
};

int __must_check vdo_initialize_super_block_codec(struct super_block_codec *codec);

void vdo_destroy_super_block_codec(struct super_block_codec *codec);

int __must_check vdo_encode_super_block(struct super_block_codec *codec);

int __must_check vdo_decode_super_block(struct super_block_codec *codec);

#endif /* SUPER_BLOCK_CODEC_H */
