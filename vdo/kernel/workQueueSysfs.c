/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium-rhel7.6/src/c++/vdo/kernel/workQueueSysfs.c#1 $
 */

#include "workQueueSysfs.h"

#include <linux/kobject.h>

#include "logger.h"
#include "memoryAlloc.h"

#include "workQueueInternals.h"

typedef struct workQueueAttribute {
  struct attribute attr;
  ssize_t (*show)(const KvdoWorkQueue *queue, char *buf);
  ssize_t (*store)(KvdoWorkQueue *queue, const char *buf, size_t length);
} WorkQueueAttribute;

/**********************************************************************/
static ssize_t nameShow(const KvdoWorkQueue *queue, char *buf)
{
  return sprintf(buf, "%s\n", queue->name);
}

/**********************************************************************/
static ssize_t pidShow(const KvdoWorkQueue *queue, char *buf)
{
  return sprintf(buf, "%ld\n",
                 (long) atomic_read(&asConstSimpleWorkQueue(queue)->threadID));
}

/**********************************************************************/
static ssize_t timesShow(const KvdoWorkQueue *queue, char *buf)
{
  return formatRunTimeStats(&asConstSimpleWorkQueue(queue)->stats, buf);
}

/**********************************************************************/
static ssize_t typeShow(const KvdoWorkQueue *queue, char *buf)
{
  strcpy(buf, queue->roundRobinMode ? "round-robin\n" : "simple\n");
  return strlen(buf);
}

/**********************************************************************/
static ssize_t workFunctionsShow(const KvdoWorkQueue *queue, char *buf)
{
  const SimpleWorkQueue *simpleQueue = asConstSimpleWorkQueue(queue);
  return formatWorkItemStats(&simpleQueue->stats.workItemStats, buf,
                             PAGE_SIZE);
}

/**********************************************************************/
static WorkQueueAttribute nameAttr = {
  .attr = { .name = "name", .mode = 0444, },
  .show = nameShow,
};

/**********************************************************************/
static WorkQueueAttribute pidAttr = {
  .attr = { .name = "pid", .mode = 0444, },
  .show = pidShow,
};

/**********************************************************************/
static WorkQueueAttribute timesAttr = {
  .attr = { .name = "times", .mode = 0444 },
  .show = timesShow,
};

/**********************************************************************/
static WorkQueueAttribute typeAttr = {
  .attr = { .name = "type", .mode = 0444, },
  .show = typeShow,
};

/**********************************************************************/
static WorkQueueAttribute workFunctionsAttr = {
  .attr = { .name = "work_functions", .mode = 0444, },
  .show = workFunctionsShow,
};

/**********************************************************************/
static struct attribute *simpleWorkQueueAttrs[] = {
  &nameAttr.attr,
  &pidAttr.attr,
  &timesAttr.attr,
  &typeAttr.attr,
  &workFunctionsAttr.attr,
  NULL,
};

/**********************************************************************/
static struct attribute *roundRobinWorkQueueAttrs[] = {
  &nameAttr.attr,
  &typeAttr.attr,
  NULL,
};

/**********************************************************************/
static ssize_t workQueueAttrShow(struct kobject   *kobj,
                                 struct attribute *attr,
                                 char             *buf)
{
  WorkQueueAttribute *wqAttr = container_of(attr, WorkQueueAttribute, attr);
  if (wqAttr->show == NULL) {
    return -EINVAL;
  }
  KvdoWorkQueue *queue = container_of(kobj, KvdoWorkQueue, kobj);
  return wqAttr->show(queue, buf);
}

/**********************************************************************/
static ssize_t workQueueAttrStore(struct kobject   *kobj,
                                  struct attribute *attr,
                                  const char       *buf,
                                  size_t            length)
{
  WorkQueueAttribute *wqAttr = container_of(attr, WorkQueueAttribute, attr);
  if (wqAttr->store == NULL) {
    return -EINVAL;
  }
  KvdoWorkQueue *queue = container_of(kobj, KvdoWorkQueue, kobj);
  return wqAttr->store(queue, buf, length);
}

/**********************************************************************/
static struct sysfs_ops workQueueSysfsOps = {
  .show  = workQueueAttrShow,
  .store = workQueueAttrStore,
};

/**********************************************************************/
static void workQueueRelease(struct kobject *kobj)
{
  KvdoWorkQueue *queue = container_of(kobj, KvdoWorkQueue, kobj);
  FREE(queue->name);
  if (queue->roundRobinMode) {
    FREE(asRoundRobinWorkQueue(queue));
  } else {
    FREE(asSimpleWorkQueue(queue));
  }
}

/**********************************************************************/
struct kobj_type simpleWorkQueueKobjType = {
  .default_attrs = simpleWorkQueueAttrs,
  .release       = workQueueRelease,
  .sysfs_ops     = &workQueueSysfsOps,
};

/**********************************************************************/
struct kobj_type roundRobinWorkQueueKobjType = {
  .default_attrs = roundRobinWorkQueueAttrs,
  .release       = workQueueRelease,
  .sysfs_ops     = &workQueueSysfsOps,
};
