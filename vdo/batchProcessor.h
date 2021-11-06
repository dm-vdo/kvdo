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

#ifndef BATCHPROCESSOR_H
#define BATCHPROCESSOR_H

#include "kernel-types.h"

/**
 * Control data for managing collections of objects to be operated on
 * by a specified function. May be used when the work function is
 * lightweight enough or cache-contentious enough that it makes sense
 * to try to accumulate multiple objects and operate on them all at
 * once in one thread.
 *
 * The work function is run in one of the kernel layer's "CPU queues",
 * and care is taken to ensure that only one invocation can be running
 * or scheduled at any given time. It can loop calling next_batch_item
 * repeatedly until there are no more objects to operate on. It should
 * also call cond_resched_batch_processor now and then, to play nicely
 * with the OS scheduler.
 *
 * Objects to operate on are manipulated through a funnel_queue_entry
 * object which must be contained within them.
 **/
struct batch_processor;

typedef void (*batch_processor_callback)(struct batch_processor *batch,
					 void *closure);

int make_batch_processor(struct vdo *vdo,
			 batch_processor_callback callback,
			 void *closure,
			 struct batch_processor **batch_ptr);

void add_to_batch_processor(struct batch_processor *batch,
			    struct vdo_work_item *item);

struct vdo_work_item * __must_check
next_batch_item(struct batch_processor *batch);

void free_batch_processor(struct batch_processor *batch);

void cond_resched_batch_processor(struct batch_processor *batch);

#endif /* BATCHPROCESSOR_H */
