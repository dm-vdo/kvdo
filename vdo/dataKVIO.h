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

#ifndef DATA_KVIO_H
#define DATA_KVIO_H

#include <linux/atomic.h>

#include "data-vio.h"

#include "kvio.h"

static inline struct data_vio *
work_item_as_data_vio(struct vdo_work_item *item)
{
	return vio_as_data_vio(work_item_as_vio(item));
}

static inline void
launch_data_vio_on_cpu_queue(struct data_vio *data_vio,
			     vdo_work_function work,
			     enum vdo_work_item_priority priority)
{
	struct vdo *vdo = vdo_from_data_vio(data_vio);

	launch_vio(data_vio_as_vio(data_vio),
		   work,
		   priority,
		   vdo->threads[vdo->thread_config->cpu_thread].queue);
}

/*
 * Enqueue a data_vio's completion's callback.
 */
static inline void enqueue_data_vio_callback(struct data_vio *data_vio)
{
	continue_vio(data_vio_as_vio(data_vio), VDO_SUCCESS);
}

struct data_location __must_check
vdo_get_dedupe_advice(const struct dedupe_context *context);
void vdo_set_dedupe_advice(struct dedupe_context *context,
			   const struct data_location *advice);

#endif /* DATA_KVIO_H */
