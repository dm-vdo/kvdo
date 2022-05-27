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
