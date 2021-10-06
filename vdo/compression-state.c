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
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/base/compression-state.c#1 $
 */

#include "compressionState.h"

#include <linux/atomic.h>

#include "dataVIO.h"
#include "kernelTypes.h"
#include "packer.h"
#include "types.h"
#include "vdo.h"
#include "vio.h"

static const uint32_t STATUS_MASK = 0xff;
static const uint32_t MAY_NOT_COMPRESS_MASK = 0x80000000;

/**********************************************************************/
struct vio_compression_state get_vio_compression_state(struct data_vio *data_vio)
{
	uint32_t packed = atomic_read(&data_vio->compression.state);

	smp_rmb();
	return (struct vio_compression_state) {
		.status = packed & STATUS_MASK,
		.may_not_compress = ((packed & MAY_NOT_COMPRESS_MASK) != 0),
	};
}

/**
 * Convert a vio_compression_state into a uint32_t which may be stored
 * atomically.
 *
 * @param state  The state to convert
 *
 * @return The compression state packed into a uint32_t
 **/
static uint32_t __must_check pack_state(struct vio_compression_state state)
{
	return state.status
	       | (state.may_not_compress ? MAY_NOT_COMPRESS_MASK : 0);
}

/**
 * Set the compression state of a data_vio.
 *
 * @param data_vio   The data_vio whose compression state is to be set
 * @param state      The expected current state of the data_vio
 * @param new_state  The state to set
 *
 * @return <code>true</code> if the new state was set, false if the data_vio's
 *         compression state did not match the expected state, and so was
 *         left unchanged
 **/
static bool __must_check
set_vio_compression_state(struct data_vio *data_vio,
			  struct vio_compression_state state,
			  struct vio_compression_state new_state)
{
	uint32_t actual;
	uint32_t expected = pack_state(state);
	uint32_t replacement = pack_state(new_state);

	// Extra barriers because this was original developed using
	// a CAS operation that implicitly had them.
	smp_mb__before_atomic();
	actual = atomic_cmpxchg(&data_vio->compression.state,
				expected, replacement);
	smp_mb__after_atomic();
	return (expected == actual);
}

/**
 * Advance to the next compression state along the compression path.
 *
 * @param data_vio  The data_vio to advance
 *
 * @return The new compression status of the data_vio
 **/
static enum vio_compression_status advance_status(struct data_vio *data_vio)
{
	for (;;) {
		struct vio_compression_state state =
			get_vio_compression_state(data_vio);
		struct vio_compression_state new_state = state;

		if (state.status == VIO_POST_PACKER) {
			// We're already in the last state.
			return state.status;
		}

		if (state.may_not_compress) {
			// Compression has been dis-allowed for this VIO, so
			// skip the rest of the path and go to the end.
			new_state.status = VIO_POST_PACKER;
		} else {
			// Go to the next state.
			new_state.status++;
		}

		if (set_vio_compression_state(data_vio, state, new_state)) {
			return new_state.status;
		}

		// Another thread changed the state out from under us so try
		// again.
	}
}

/**********************************************************************/
bool may_compress_data_vio(struct data_vio *data_vio)
{
	if (!data_vio_has_allocation(data_vio) ||
	    vio_requires_flush_after(data_vio_as_vio(data_vio)) ||
	    !get_vdo_compressing(get_vdo_from_data_vio(data_vio))) {
		/*
		 * If this VIO didn't get an allocation, the compressed write
		 * probably won't either, so don't try compressing it. Also, if
		 * compression is off, don't compress.
		 */
		set_vio_compression_done(data_vio);
		return false;
	}

	if (data_vio->hash_lock == NULL) {
		/*
		 * data_vios without a hash_lock (which should be extremely
		 * rare) aren't able to share the packer's PBN lock, so don't
		 * try to compress them.
                 */
		set_vio_compression_done(data_vio);
		return false;
	}

	/*
	 * If the orignal bio was a discard, but we got this far because the
	 * discard was a partial one (r/m/w), and it is part of a larger
	 * discard, we cannot compress this vio. We need to make sure the vio
	 * completes ASAP.
         *
         * XXX: given the hash lock bailout, is this even possible?
	 */
	if ((data_vio->user_bio != NULL) &&
	    (bio_op(data_vio->user_bio) == REQ_OP_DISCARD) &&
	    (data_vio->remaining_discard > 0)) {
		set_vio_compression_done(data_vio);
		return false;
	}

	return (advance_status(data_vio) == VIO_COMPRESSING);
}

/**********************************************************************/
bool may_pack_data_vio(struct data_vio *data_vio)
{
	if (!vdo_data_is_sufficiently_compressible(data_vio) ||
	    !get_vdo_compressing(get_vdo_from_data_vio(data_vio)) ||
	    get_vio_compression_state(data_vio).may_not_compress) {
		// If the data in this VIO doesn't compress, or compression is
		// off, or compression for this VIO has been canceled, don't
		// send it to the packer.
		set_vio_compression_done(data_vio);
		return false;
	}

	return true;
}

/**********************************************************************/
bool may_vio_block_in_packer(struct data_vio *data_vio)
{
	return (advance_status(data_vio) == VIO_PACKING);
}

/**********************************************************************/
bool may_write_compressed_data_vio(struct data_vio *data_vio)
{
	advance_status(data_vio);
	return !get_vio_compression_state(data_vio).may_not_compress;
}

/**********************************************************************/
void set_vio_compression_done(struct data_vio *data_vio)
{
	for (;;) {
		struct vio_compression_state new_state = {
			.status = VIO_POST_PACKER,
			.may_not_compress = true,
		};
		struct vio_compression_state state =
			get_vio_compression_state(data_vio);

		if (state.status == VIO_POST_PACKER) {
			// The VIO is already done.
			return;
		}

		// If compression was cancelled on this VIO, preserve that fact.
		if (set_vio_compression_state(data_vio, state, new_state)) {
			return;
		}
	}
}

/**********************************************************************/
bool cancel_vio_compression(struct data_vio *data_vio)
{
	struct vio_compression_state state, new_state;

	for (;;) {
		state = get_vio_compression_state(data_vio);
		if (state.may_not_compress ||
		    (state.status == VIO_POST_PACKER)) {
			// This data_vio is already set up to not block in the
			// packer.
			break;
		}

		new_state.status = state.status;
		new_state.may_not_compress = true;

		if (set_vio_compression_state(data_vio, state, new_state)) {
			break;
		}
	}

	return ((state.status == VIO_PACKING) && !state.may_not_compress);
}
