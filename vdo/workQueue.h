/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_WORK_QUEUE_H
#define VDO_WORK_QUEUE_H

#include <linux/sched.h> /* for TASK_COMM_LEN */

#include "funnel-queue.h"

#include "kernel-types.h"
#include "types.h"

enum {
	MAX_VDO_WORK_QUEUE_NAME_LEN = TASK_COMM_LEN,
};

struct vdo_work_queue_type {
	void (*start)(void *);
	void (*finish)(void *);
	enum vdo_completion_priority max_priority;
	enum vdo_completion_priority default_priority;
};

int make_work_queue(const char *thread_name_prefix,
		    const char *name,
		    struct vdo_thread *owner,
		    const struct vdo_work_queue_type *type,
		    unsigned int thread_count,
		    void *thread_privates[],
		    struct vdo_work_queue **queue_ptr);

void enqueue_work_queue(struct vdo_work_queue *queue,
			struct vdo_completion *completion);

void finish_work_queue(struct vdo_work_queue *queue);

void free_work_queue(struct vdo_work_queue *queue);

void dump_work_queue(struct vdo_work_queue *queue);

void dump_completion_to_buffer(struct vdo_completion *completion,
			       char *buffer,
			       size_t length);

void *get_work_queue_private_data(void);
struct vdo_work_queue *get_current_work_queue(void);
struct vdo_thread *get_work_queue_owner(struct vdo_work_queue *queue);

bool __must_check
vdo_work_queue_type_is(struct vdo_work_queue *queue,
		       const struct vdo_work_queue_type *type);

#endif /* VDO_WORK_QUEUE_H */
