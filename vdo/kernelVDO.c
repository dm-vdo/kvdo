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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.c#129 $
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

#include "read-only-notifier.h"
#include "statistics.h"
#include "thread-config.h"
#include "vdo.h"
#include "vdo-load.h"
#include "vdo-resize.h"
#include "vdo-resize-logical.h"
#include "vdo-resume.h"
#include "vdo-suspend.h"
#include "workQueue.h"

#include "kvio.h"

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
	struct vdo_completion *completion = vio_as_completion(vio);
	thread_id_t thread_id = completion->callback_thread_id;

	BUG_ON(thread_id >= completion->vdo->thread_config->thread_count);
	launch_vio(vio,
		   work,
		   priority,
		   completion->vdo->threads[thread_id].queue);
}
