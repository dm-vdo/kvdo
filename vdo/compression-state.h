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

#ifndef COMPRESSION_STATE_H
#define COMPRESSION_STATE_H

#include "kernel-types.h"
#include "types.h"

/**
 * Where a data_vio is on the compression path; advance_status() depends on the
 * order of this enum.
 **/
enum vio_compression_status {
	/* A VIO which has not yet entered the compression path */
	VIO_PRE_COMPRESSOR,
	/* A VIO which is in the compressor */
	VIO_COMPRESSING,
	/* A VIO which is blocked in the packer */
	VIO_PACKING,
	/*
	 * A VIO which is no longer on the compression path (and never will be)
	 */
	VIO_POST_PACKER,
};

struct vio_compression_state {
	enum vio_compression_status status;
	bool may_not_compress;
};

struct vio_compression_state __must_check
get_vio_compression_state(struct data_vio *data_vio);

bool __must_check may_compress_data_vio(struct data_vio *data_vio);

bool __must_check may_pack_data_vio(struct data_vio *data_vio);

bool __must_check may_vio_block_in_packer(struct data_vio *data_vio);

bool __must_check may_write_compressed_data_vio(struct data_vio *data_vio);

void set_vio_compression_done(struct data_vio *data_vio);

bool cancel_vio_compression(struct data_vio *data_vio);

#endif /* COMPRESSION_STATE_H */
