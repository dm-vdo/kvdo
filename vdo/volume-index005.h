/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VOLUMEINDEX005_H
#define VOLUMEINDEX005_H 1

#include "volume-index-ops.h"

/**
 * Make a new volume index.
 *
 * @param config        The configuration of the volume index
 * @param volume_nonce  The nonce used to authenticate the index
 * @param volume_index  Location to hold new volume index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check make_volume_index005(const struct configuration *config,
				      uint64_t volume_nonce,
				      struct volume_index **volume_index);

/**
 * Compute the number of bytes required to save a volume index of a given
 * configuration.
 *
 * @param config     The configuration of the volume index
 * @param num_bytes  The number of bytes required to save the volume index
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
compute_volume_index_save_bytes005(const struct configuration *config,
				   size_t *num_bytes);

#endif /* VOLUMEINDEX005_H */
