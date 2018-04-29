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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/poolSysfs.c#1 $
 */

#include "poolSysfs.h"

#include "memoryAlloc.h"

#include "vdo.h"

#include "dedupeIndex.h"

typedef struct poolAttribute {
  struct attribute attr;
  ssize_t (*show)(KernelLayer *layer, char *buf);
  ssize_t (*store)(KernelLayer *layer, const char *value, size_t count);
} PoolAttribute;

/**********************************************************************/
static ssize_t vdoPoolAttrShow(struct kobject   *kobj,
                               struct attribute *attr,
                               char             *buf)
{
  PoolAttribute *poolAttr = container_of(attr, PoolAttribute, attr);
  if (poolAttr->show == NULL) {
    return -EINVAL;
  }
  KernelLayer *layer = container_of(kobj, KernelLayer, kobj);
  return poolAttr->show(layer, buf);
}

/**********************************************************************/
static ssize_t vdoPoolAttrStore(struct kobject   *kobj,
                                struct attribute *attr,
                                const char       *buf,
                                size_t            length)
{
  PoolAttribute *poolAttr = container_of(attr, PoolAttribute, attr);
  if (poolAttr->store == NULL) {
    return -EINVAL;
  }
  KernelLayer *layer = container_of(kobj, KernelLayer, kobj);
  return poolAttr->store(layer, buf, length);
}

static struct sysfs_ops vdoPoolSysfsOps = {
  .show  = vdoPoolAttrShow,
  .store = vdoPoolAttrStore,
};

/**********************************************************************/
static ssize_t poolCompressingShow(KernelLayer *layer, char *buf)
{
  return sprintf(buf, "%s\n", (getKVDOCompressing(&layer->kvdo) ? "1" : "0"));
}

/**********************************************************************/
static ssize_t poolDiscardsActiveShow(KernelLayer *layer, char *buf)
{
  return sprintf(buf, "%" PRIu32 "\n", layer->discardLimiter.active);
}

/**********************************************************************/
static ssize_t poolDiscardsLimitShow(KernelLayer *layer, char *buf)
{
  return sprintf(buf, "%" PRIu32 "\n", layer->discardLimiter.limit);
}

/**********************************************************************/
static ssize_t poolDiscardsLimitStore(KernelLayer *layer,
                                      const char  *buf,
                                      size_t       length)
{
  unsigned int value;
  if ((length > 12) || (sscanf(buf, "%u", &value) != 1) || (value < 1)) {
    return -EINVAL;
  }
  layer->discardLimiter.limit = value;
  return length;
}

/**********************************************************************/
static ssize_t poolDiscardsMaximumShow(KernelLayer *layer, char *buf)
{
  return sprintf(buf, "%" PRIu32 "\n", layer->discardLimiter.maximum);
}

/**********************************************************************/
static ssize_t poolInstanceShow(KernelLayer *layer, char *buf)
{
  return sprintf(buf, "%u\n", layer->instance);
}

/**********************************************************************/
static ssize_t poolRequestsActiveShow(KernelLayer *layer, char *buf)
{
  return sprintf(buf, "%" PRIu32 "\n", layer->requestLimiter.active);
}

/**********************************************************************/
static ssize_t poolRequestsLimitShow(KernelLayer *layer, char *buf)
{
  return sprintf(buf, "%" PRIu32 "\n", layer->requestLimiter.limit);
}

/**********************************************************************/
static ssize_t poolRequestsMaximumShow(KernelLayer *layer, char *buf)
{
  return sprintf(buf, "%" PRIu32 "\n", layer->requestLimiter.maximum);
}

/**********************************************************************/
static void vdoPoolRelease(struct kobject *kobj)
{
  KernelLayer *layer = container_of(kobj, KernelLayer, kobj);
  FREE(layer->vdoStatsStorage);
  FREE(layer->kernelStatsStorage);
  freeVDO(&layer->kvdo.vdo);
  FREE(layer);
}

static PoolAttribute vdoPoolCompressingAttr = {
  .attr  = { .name = "compressing", .mode = 0444, },
  .show  = poolCompressingShow,
};

static PoolAttribute vdoPoolDiscardsActiveAttr = {
  .attr  = { .name = "discards_active", .mode = 0444, },
  .show  = poolDiscardsActiveShow,
};

static PoolAttribute vdoPoolDiscardsLimitAttr = {
  .attr  = { .name = "discards_limit", .mode = 0644, },
  .show  = poolDiscardsLimitShow,
  .store = poolDiscardsLimitStore,
};

static PoolAttribute vdoPoolDiscardsMaximumAttr = {
  .attr  = { .name = "discards_maximum", .mode = 0444, },
  .show  = poolDiscardsMaximumShow,
};

static PoolAttribute vdoPoolInstanceAttr = {
  .attr  = { .name = "instance", .mode = 0444, },
  .show  = poolInstanceShow,
};

static PoolAttribute vdoPoolRequestsActiveAttr = {
  .attr  = { .name = "requests_active", .mode = 0444, },
  .show  = poolRequestsActiveShow,
};

static PoolAttribute vdoPoolRequestsLimitAttr = {
  .attr  = { .name = "requests_limit", .mode = 0444, },
  .show  = poolRequestsLimitShow,
};

static PoolAttribute vdoPoolRequestsMaximumAttr = {
  .attr  = { .name = "requests_maximum", .mode = 0444, },
  .show  = poolRequestsMaximumShow,
};

static struct attribute *poolAttrs[] = {
  &vdoPoolCompressingAttr.attr,
  &vdoPoolDiscardsActiveAttr.attr,
  &vdoPoolDiscardsLimitAttr.attr,
  &vdoPoolDiscardsMaximumAttr.attr,
  &vdoPoolInstanceAttr.attr,
  &vdoPoolRequestsActiveAttr.attr,
  &vdoPoolRequestsLimitAttr.attr,
  &vdoPoolRequestsMaximumAttr.attr,
  NULL,
};

struct kobj_type kernelLayerKobjType = {
  .release       = vdoPoolRelease,
  .sysfs_ops     = &vdoPoolSysfsOps,
  .default_attrs = poolAttrs,
};

/**********************************************************************/
static void workQueueDirectoryRelease(struct kobject *kobj)
{
  /*
   * The workQueueDirectory holds an implicit reference to its parent,
   * the kernelLayer object (->kobj), so even if there are some
   * external references held to the workQueueDirectory when work
   * queue shutdown calls kobject_put on the kernelLayer object, the
   * kernelLayer object won't actually be released and won't free the
   * KernelLayer storage until the workQueueDirectory object is
   * released first.
   *
   * So, we don't need to do any additional explicit management here.
   *
   * (But we aren't allowed to use a NULL function pointer to indicate
   * a no-op.)
   */
}

/**********************************************************************/
static struct attribute *noAttrs[] = {
  NULL,
};

static struct sysfs_ops noSysfsOps = {
  // These should never be reachable since there are no attributes.
  .show  = NULL,
  .store = NULL,
};

struct kobj_type workQueueDirectoryKobjType = {
  .release       = workQueueDirectoryRelease,
  .sysfs_ops     = &noSysfsOps,
  .default_attrs = noAttrs,
};
