/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 */

#ifndef DATA_VIO_POOL_H
#define DATA_VIO_POOL_H

#include <linux/bio.h>

#include "kernel-types.h"
#include "types.h"

int make_data_vio_pool(struct vdo *vdo,
		       vio_count_t pool_size,
		       vio_count_t discard_limit,
		       struct data_vio_pool **pool_ptr);

void free_data_vio_pool(struct data_vio_pool *pool);

void vdo_launch_bio(struct data_vio_pool *pool, struct bio *bio);

void release_data_vio(struct data_vio *data_vio);

void drain_data_vio_pool(struct data_vio_pool *pool,
			 struct vdo_completion *completion);

void resume_data_vio_pool(struct data_vio_pool *pool,
			  struct vdo_completion *completion);

void dump_data_vio_pool(struct data_vio_pool *pool, bool dump_vios);

vio_count_t get_data_vio_pool_active_discards(struct data_vio_pool *pool);
vio_count_t get_data_vio_pool_discard_limit(struct data_vio_pool *pool);
vio_count_t get_data_vio_pool_maximum_discards(struct data_vio_pool *pool);
int __must_check set_data_vio_pool_discard_limit(struct data_vio_pool *pool,
						 vio_count_t limit);
vio_count_t get_data_vio_pool_active_requests(struct data_vio_pool *pool);
vio_count_t get_data_vio_pool_request_limit(struct data_vio_pool *pool);
vio_count_t get_data_vio_pool_maximum_requests(struct data_vio_pool *pool);

#endif // DATA_VIO_POOL_H
