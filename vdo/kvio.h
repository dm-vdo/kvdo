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

#ifndef KVIO_H
#define KVIO_H

#include "vio.h"

#include "workQueue.h"

static inline void enqueue_vio_work(struct vdo_work_queue *queue,
				    struct vio *vio)
{
	enqueue_work_queue(queue, work_item_from_vio(vio));
}

static inline void setup_vio_work(struct vio *vio,
				  vdo_work_function work,
				  enum vdo_work_item_priority priority)
{
	setup_work_item(work_item_from_vio(vio),
			work,
			priority);
}

static inline void launch_vio(struct vio *vio,
			      vdo_work_function work,
			      enum vdo_work_item_priority priority,
			      struct vdo_work_queue *queue)
{
	setup_vio_work(vio, work, priority);
	enqueue_vio_work(queue, vio);
}

#endif /* KVIO_H */
