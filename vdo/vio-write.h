/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VIO_WRITE_H
#define VIO_WRITE_H

#include "kernel-types.h"

void launch_write_data_vio(struct data_vio *data_vio);

void cleanup_write_data_vio(struct data_vio *data_vio);

void continue_write_after_compression(struct data_vio *data_vio);

void launch_compress_data_vio(struct data_vio *data_vio);

void launch_deduplicate_data_vio(struct data_vio *data_vio);

#endif /* VIO_WRITE_H */
