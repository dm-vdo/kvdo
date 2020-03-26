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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/compressionState.c#10 $
 */

#include "compressionStateInternals.h"

#include "dataVIO.h"
#include "packer.h"

static const uint32_t STATUS_MASK = 0xff;
static const uint32_t MAY_NOT_COMPRESS_MASK = 0x80000000;

/**********************************************************************/
struct vio_compression_state get_compression_state(struct data_vio *data_vio)
{
	uint32_t packedValue = atomicLoad32(&data_vio->compression.state);
	return (struct vio_compression_state) {
		.status = packedValue & STATUS_MASK,
		.may_not_compress = ((packedValue & MAY_NOT_COMPRESS_MASK) != 0),
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
__attribute__((warn_unused_result)) static uint32_t
packState(struct vio_compression_state state)
{
	return state.status
	       | (state.may_not_compress ? MAY_NOT_COMPRESS_MASK : 0);
}

/**********************************************************************/
bool set_compression_state(struct data_vio *data_vio,
			   struct vio_compression_state state,
			   struct vio_compression_state new_state)
{
	return compareAndSwap32(&data_vio->compression.state, packState(state),
				packState(new_state));
}

/**
 * Advance to the next compression state along the compression path.
 *
 * @param data_vio  The data_vio to advance
 *
 * @return The new compression status of the data_vio
 **/
static vio_compression_status advance_status(struct data_vio *data_vio)
{
	for (;;) {
		struct vio_compression_state state =
			get_compression_state(data_vio);
		if (state.status == VIO_POST_PACKER) {
			// We're already in the last state.
			return state.status;
		}

		struct vio_compression_state new_state = state;
		if (state.may_not_compress) {
			// Compression has been dis-allowed for this VIO, so
			// skip the rest of the path and go to the end.
			new_state.status = VIO_POST_PACKER;
		} else {
			// Go to the next state.
			new_state.status++;
		}

		if (set_compression_state(data_vio, state, new_state)) {
			return new_state.status;
		}

		// Another thread changed the state out from under us so try
		// again.
	}
}

/**********************************************************************/
bool may_compress_data_vio(struct data_vio *data_vio)
{
	if (!has_allocation(data_vio) ||
	    ((get_write_policy(get_vdo_from_data_vio(data_vio)) !=
	    	WRITE_POLICY_SYNC) &&
	        vioRequiresFlushAfter(data_vio_as_vio(data_vio))) ||
	    !get_vdo_compressing(get_vdo_from_data_vio(data_vio))) {
		/*
		 * If this VIO didn't get an allocation, the compressed write
		 * probably won't either, so don't try compressing it. Also, if
		 * compression is off, don't compress.
		 */
		set_compression_done(data_vio);
		return false;
	}

	if (data_vio->hashLock == NULL) {
		// DataVIOs without a HashLock (which should be extremely rare)
		// aren't able to share the packer's PBN lock, so don't try to
		// compress them.
		return false;
	}

	return (advance_status(data_vio) == VIO_COMPRESSING);
}

/**********************************************************************/
bool may_pack_data_vio(struct data_vio *data_vio)
{
	if (!is_sufficiently_compressible(data_vio) ||
	    !get_vdo_compressing(get_vdo_from_data_vio(data_vio)) ||
	    get_compression_state(data_vio).may_not_compress) {
		// If the data in this VIO doesn't compress, or compression is
		// off, or compression for this VIO has been canceled, don't
		// send it to the packer.
		set_compression_done(data_vio);
		return false;
	}

	return true;
}

/**********************************************************************/
bool may_block_in_packer(struct data_vio *data_vio)
{
	return (advance_status(data_vio) == VIO_PACKING);
}

/**********************************************************************/
bool may_write_compressed_data_vio(struct data_vio *data_vio)
{
	advance_status(data_vio);
	return !get_compression_state(data_vio).may_not_compress;
}

/**********************************************************************/
void set_compression_done(struct data_vio *data_vio)
{
	for (;;) {
		struct vio_compression_state state =
			get_compression_state(data_vio);
		if (state.status == VIO_POST_PACKER) {
			// The VIO is already done.
			return;
		}

		// If compression was cancelled on this VIO, preserve that fact.
		struct vio_compression_state new_state = {
			.status = VIO_POST_PACKER,
			.may_not_compress = true,
		};
		if (set_compression_state(data_vio, state, new_state)) {
			return;
		}
	}
}

/**********************************************************************/
bool cancel_compression(struct data_vio *data_vio)
{
	struct vio_compression_state state;
	for (;;) {
		state = get_compression_state(data_vio);
		if (state.may_not_compress ||
		    (state.status == VIO_POST_PACKER)) {
			// This data_vio is already set up to not block in the
			// packer.
			break;
		}

		struct vio_compression_state new_state = {
			.status = state.status,
			.may_not_compress = true,
		};
		if (set_compression_state(data_vio, state, new_state)) {
			break;
		}
	}

	return ((state.status == VIO_PACKING) && !state.may_not_compress);
}
