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

#ifndef IO_SUBMITTER_H
#define IO_SUBMITTER_H

#include <linux/bio.h>

#include "kernel-types.h"

int vdo_make_io_submitter(const char *thread_name_prefix,
			  unsigned int thread_count,
			  unsigned int rotation_interval,
			  unsigned int max_requests_active,
			  struct vdo *vdo,
			  struct io_submitter **io_submitter);

void vdo_cleanup_io_submitter(struct io_submitter *io_submitter);

void vdo_free_io_submitter(struct io_submitter *io_submitter);

void process_data_vio_io(struct vdo_completion *completion);

void submit_data_vio_io(struct data_vio *data_vio);

void vdo_submit_bio(struct bio *bio, enum vdo_work_item_priority priority);

#endif /* IO_SUBMITTER_H */
