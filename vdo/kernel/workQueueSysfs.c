/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueueSysfs.c#9 $
 */

#include "workQueueSysfs.h"

#include <linux/kobject.h>

#include "logger.h"
#include "memoryAlloc.h"

#include "workQueueInternals.h"

struct work_queue_attribute {
	struct attribute attr;
	ssize_t (*show)(const struct kvdo_work_queue *queue, char *buf);
	ssize_t (*store)(struct kvdo_work_queue *queue,
			 const char *buf,
			 size_t length);
};

/**********************************************************************/
static ssize_t name_show(const struct kvdo_work_queue *queue, char *buf)
{
	return sprintf(buf, "%s\n", queue->name);
}

/**********************************************************************/
static ssize_t pid_show(const struct kvdo_work_queue *queue, char *buf)
{
	return sprintf(buf,
		       "%ld\n",
		       (long) atomic_read(&as_const_simple_work_queue(queue)->thread_id));
}

/**********************************************************************/
static ssize_t times_show(const struct kvdo_work_queue *queue, char *buf)
{
	return format_run_time_stats(&as_const_simple_work_queue(queue)->stats,
				     buf);
}

/**********************************************************************/
static ssize_t type_show(const struct kvdo_work_queue *queue, char *buf)
{
	strcpy(buf, queue->round_robin_mode ? "round-robin\n" : "simple\n");
	return strlen(buf);
}

/**********************************************************************/
static ssize_t work_functions_show(const struct kvdo_work_queue *queue,
				   char *buf)
{
	const struct simple_work_queue *simple_queue =
		as_const_simple_work_queue(queue);
	return format_work_item_stats(&simple_queue->stats.work_item_stats,
				      buf,
				      PAGE_SIZE);
}

/**********************************************************************/
static struct work_queue_attribute name_attr = {
	.attr = {

			.name = "name",
			.mode = 0444,
		},
	.show = name_show,
};

/**********************************************************************/
static struct work_queue_attribute pid_attr = {
	.attr = {

			.name = "pid",
			.mode = 0444,
		},
	.show = pid_show,
};

/**********************************************************************/
static struct work_queue_attribute times_attr = {
	.attr = {

			.name = "times",
			.mode = 0444
		},
	.show = times_show,
};

/**********************************************************************/
static struct work_queue_attribute type_attr = {
	.attr = {

			.name = "type",
			.mode = 0444,
		},
	.show = type_show,
};

/**********************************************************************/
static struct work_queue_attribute work_functions_attr = {
	.attr = {

			.name = "work_functions",
			.mode = 0444,
		},
	.show = work_functions_show,
};

/**********************************************************************/
static struct attribute *simple_work_queue_attrs[] = {
	&name_attr.attr,
	&pid_attr.attr,
	&times_attr.attr,
	&type_attr.attr,
	&work_functions_attr.attr,
	NULL,
};

/**********************************************************************/
static struct attribute *round_robin_work_queue_attrs[] = {
	&name_attr.attr,
	&type_attr.attr,
	NULL,
};

/**********************************************************************/
static ssize_t work_queue_attr_show(struct kobject *kobj,
				    struct attribute *attr, char *buf)
{
	struct work_queue_attribute *wq_attr =
		container_of(attr, struct work_queue_attribute, attr);
	if (wq_attr->show == NULL) {
		return -EINVAL;
	}
	struct kvdo_work_queue *queue = container_of(kobj,
						     struct kvdo_work_queue,
						     kobj);
	return wq_attr->show(queue, buf);
}

/**********************************************************************/
static ssize_t work_queue_attr_store(struct kobject *kobj,
				     struct attribute *attr, const char *buf,
				     size_t length)
{
	struct work_queue_attribute *wq_attr =
		container_of(attr, struct work_queue_attribute, attr);
	if (wq_attr->store == NULL) {
		return -EINVAL;
	}
	struct kvdo_work_queue *queue =
		container_of(kobj, struct kvdo_work_queue, kobj);
	return wq_attr->store(queue, buf, length);
}

/**********************************************************************/
static struct sysfs_ops work_queue_sysfs_ops = {
	.show = work_queue_attr_show,
	.store = work_queue_attr_store,
};

/**********************************************************************/
static void work_queue_release(struct kobject *kobj)
{
	struct kvdo_work_queue *queue =
		container_of(kobj, struct kvdo_work_queue, kobj);
	FREE(queue->name);
	if (queue->round_robin_mode) {
		FREE(as_round_robin_work_queue(queue));
	} else {
		FREE(as_simple_work_queue(queue));
	}
}

/**********************************************************************/
struct kobj_type simple_work_queue_kobj_type = {
	.default_attrs = simple_work_queue_attrs,
	.release = work_queue_release,
	.sysfs_ops = &work_queue_sysfs_ops,
};

/**********************************************************************/
struct kobj_type round_robin_work_queue_kobj_type = {
	.default_attrs = round_robin_work_queue_attrs,
	.release = work_queue_release,
	.sysfs_ops = &work_queue_sysfs_ops,
};
