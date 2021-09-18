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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.c#126 $
 */

/*
 * Sadly, this include must precede the include of kernelVDOInternals.h because
 * that file ends up including the uds version of errors.h which is wrong for
 * this file.
 */
#include "errors.h"
#include "kernelVDO.h"

#include <linux/delay.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "readOnlyNotifier.h"
#include "statistics.h"
#include "threadConfig.h"
#include "vdo.h"
#include "vdoLoad.h"
#include "vdoResize.h"
#include "vdoResizeLogical.h"
#include "vdoResume.h"
#include "vdoSuspend.h"
#include "workQueue.h"

#include "kvio.h"

enum { PARANOID_THREAD_CONSISTENCY_CHECKS = 0 };

/**********************************************************************/
void enqueue_vdo_work(struct vdo *vdo,
		      struct vdo_work_item *item,
		      thread_id_t thread_id)
{
	enqueue_work_queue(vdo->threads[thread_id].queue, item);
}

/**********************************************************************/
void enqueue_vio(struct vio *vio,
		 vdo_work_function work,
		 enum vdo_work_item_priority priority)
{
	thread_id_t thread_id = vio_as_completion(vio)->callback_thread_id;

	BUG_ON(thread_id >= vio->vdo->thread_config->thread_count);
	launch_vio(vio,
		   work,
		   priority,
		   vio->vdo->threads[thread_id].queue);
}

/**********************************************************************/
thread_id_t vdo_get_callback_thread_id(void)
{
	struct vdo_work_queue *queue = get_current_work_queue();
	struct vdo_thread *thread;
	thread_id_t thread_id;

	if (queue == NULL) {
		return VDO_INVALID_THREAD_ID;
	}

	thread = get_work_queue_owner(queue);
	thread_id = thread->thread_id;

	if (PARANOID_THREAD_CONSISTENCY_CHECKS) {
		struct vdo *vdo = thread->vdo;

		BUG_ON(thread_id >= vdo->thread_config->thread_count);
		BUG_ON(thread != &vdo->threads[thread_id]);
	}

	return thread_id;
}
