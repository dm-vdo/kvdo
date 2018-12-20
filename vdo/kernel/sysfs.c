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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/sysfs.c#2 $
 */

#include "sysfs.h"

#include <linux/module.h>
#include <linux/version.h>

#include "dedupeIndex.h"
#include "dmvdo.h"
#include "logger.h"

extern int defaultMaxRequestsActive;

typedef struct vdoAttribute {
  struct attribute                             attr;
  ssize_t (*show)(struct kvdo_module_globals  *kvdoGlobals,
                  struct attribute            *attr,
                  char                        *buf);
  ssize_t (*store)(struct kvdo_module_globals *kvdoGlobals,
                   const char                 *value,
                   size_t                      count);
  // Location of value, if .show == showInt or showUInt or showBool.
  void                                        *valuePtr;
} VDOAttribute;

static char *statusStrings[] = {
  "UNINITIALIZED",
  "READY",
  "SHUTTING DOWN",
};

/**********************************************************************/
static ssize_t vdoStatusShow(struct kvdo_module_globals *kvdoGlobals,
                             struct attribute           *attr,
                             char                       *buf)
{
  return sprintf(buf, "%s\n", statusStrings[kvdoGlobals->status]);
}

/**********************************************************************/
static ssize_t vdoLogLevelShow(struct kvdo_module_globals *kvdoGlobals,
                               struct attribute           *attr,
                               char                       *buf)
{
  return sprintf(buf, "%s\n", priorityToString(getLogLevel()));
}

/**********************************************************************/
static ssize_t vdoLogLevelStore(struct kvdo_module_globals *kvdoGlobals,
                                const char                 *buf,
                                size_t                      n)
{
  static char internalBuf[11];

  if (n > 10) {
    return -EINVAL;
  }

  memset(internalBuf, '\000', sizeof(internalBuf));
  memcpy(internalBuf, buf, n);
  if (internalBuf[n - 1] == '\n') {
    internalBuf[n - 1] = '\000';
  }
  setLogLevel(stringToPriority(internalBuf));
  return n;
}

/**********************************************************************/
static ssize_t scanInt(const char *buf,
                       size_t      n,
                       int        *valuePtr,
                       int         minimum,
                       int         maximum)
{
  if (n > 12) {
    return -EINVAL;
  }
  unsigned int value;
  if (sscanf(buf, "%d", &value) != 1) {
    return -EINVAL;
  }
  if (value < minimum) {
    value = minimum;
  } else if (value > maximum) {
    value = maximum;
  }
  *valuePtr = value;
  return n;
}

/**********************************************************************/
static ssize_t showInt(struct kvdo_module_globals *kvdoGlobals,
                       struct attribute           *attr,
                       char                       *buf)
{
  VDOAttribute *vdoAttr = container_of(attr, VDOAttribute, attr);

  return sprintf(buf, "%d\n", *(int *)vdoAttr->valuePtr);
}

/**********************************************************************/
static ssize_t scanUInt(const char   *buf,
                        size_t        n,
                        unsigned int *valuePtr,
                        unsigned int  minimum,
                        unsigned int  maximum)
{
  if (n > 12) {
    return -EINVAL;
  }
  unsigned int value;
  if (sscanf(buf, "%u", &value) != 1) {
    return -EINVAL;
  }
  if (value < minimum) {
    value = minimum;
  } else if (value > maximum) {
    value = maximum;
  }
  *valuePtr = value;
  return n;
}

/**********************************************************************/
static ssize_t showUInt(struct kvdo_module_globals *kvdoGlobals,
                        struct attribute           *attr,
                        char                       *buf)
{
  VDOAttribute *vdoAttr = container_of(attr, VDOAttribute, attr);

  return sprintf(buf, "%u\n", *(unsigned int *)vdoAttr->valuePtr);
}

/**********************************************************************/
static ssize_t scanBool(const char *buf, size_t n, bool *valuePtr)
{
  unsigned int intValue = 0;
  n = scanUInt(buf, n, &intValue, 0, 1);
  if (n > 0) {
    *valuePtr = (intValue != 0);
  }
  return n;
}

/**********************************************************************/
static ssize_t showBool(struct kvdo_module_globals *kvdoGlobals,
                        struct attribute           *attr,
                        char                       *buf)
{
  VDOAttribute *vdoAttr = container_of(attr, VDOAttribute, attr);

  return sprintf(buf, "%u\n", *(bool *)vdoAttr->valuePtr ? 1 : 0);
}

/**********************************************************************/
static ssize_t vdoTraceRecordingStore(struct kvdo_module_globals *kvdoGlobals,
                                      const char                 *buf,
                                      size_t                      n)
{
  return scanBool(buf, n, &traceRecording);
}

/**********************************************************************/
static ssize_t vdoMaxReqActiveStore(struct kvdo_module_globals *kvdoGlobals,
                                    const char                 *buf,
                                    size_t                      n)
{
  /*
   * The base code has some hardcoded assumptions about the maximum
   * number of requests that can be in progress. Maybe someday we'll
   * do calculations with the actual number; for now, just make sure
   * the assumption holds.
   */
  return scanInt(buf, n, &defaultMaxRequestsActive, 1, MAXIMUM_USER_VIOS);
}

/**********************************************************************/
static ssize_t vdoAlbireoTimeoutIntervalStore(struct kvdo_module_globals *kvdoGlobals,
                                              const char                 *buf,
                                              size_t                      n)
{
  unsigned int value;
  ssize_t result = scanUInt(buf, n, &value, 0, UINT_MAX);
  if (result > 0) {
    setAlbireoTimeoutInterval(value);
  }
  return result;
}

/**********************************************************************/
static ssize_t vdoMinAlbireoTimerIntervalStore(struct kvdo_module_globals *kvdoGlobals,
                                               const char                 *buf,
                                               size_t                      n)
{
  unsigned int value;
  ssize_t result = scanUInt(buf, n, &value, 0, UINT_MAX);
  if (result > 0) {
    setMinAlbireoTimerInterval(value);
  }
  return result;
}

/**********************************************************************/
static ssize_t vdoVersionShow(struct kvdo_module_globals *kvdoGlobals,
                              struct attribute           *attr,
                              char                       *buf)
{
  return sprintf(buf, "%s\n", CURRENT_VERSION);
}

/**********************************************************************/
static ssize_t vdoAttrShow(struct kobject   *kobj,
                           struct attribute *attr,
                           char             *buf)
{
  VDOAttribute *vdoAttr = container_of(attr, VDOAttribute, attr);
  if (vdoAttr->show == NULL) {
    return -EINVAL;
  }

  struct kvdo_module_globals *kvdoGlobals;
  kvdoGlobals = container_of(kobj, struct kvdo_module_globals, kobj);
  return (*vdoAttr->show)(kvdoGlobals, attr, buf);
}

/**********************************************************************/
static ssize_t vdoAttrStore(struct kobject   *kobj,
                            struct attribute *attr,
                            const char       *buf,
                            size_t            length)
{
  VDOAttribute *vdoAttr = container_of(attr, VDOAttribute, attr);
  if (vdoAttr->store == NULL) {
    return -EINVAL;
  }

  struct kvdo_module_globals *kvdoGlobals;
  kvdoGlobals = container_of(kobj, struct kvdo_module_globals, kobj);
  return (*vdoAttr->store)(kvdoGlobals, buf, length);
}

static VDOAttribute vdoStatusAttr = {
  .attr  = { .name = "status", .mode = 0444, },
  .show  = vdoStatusShow,
};

static VDOAttribute vdoLogLevelAttr = {
  .attr  = {.name = "log_level", .mode = 0644, },
  .show  = vdoLogLevelShow,
  .store = vdoLogLevelStore,
};

static VDOAttribute vdoMaxReqActiveAttr = {
  .attr     = {.name = "max_requests_active", .mode = 0644, },
  .show     = showInt,
  .store    = vdoMaxReqActiveStore,
  .valuePtr = &defaultMaxRequestsActive,
};

static VDOAttribute vdoAlbireoTimeoutInterval = {
  .attr     = {.name = "deduplication_timeout_interval", .mode = 0644, },
  .show     = showUInt,
  .store    = vdoAlbireoTimeoutIntervalStore,
  .valuePtr = &albireoTimeoutInterval,
};

static VDOAttribute vdoMinAlbireoTimerInterval = {
  .attr     = {.name = "min_deduplication_timer_interval", .mode = 0644, },
  .show     = showUInt,
  .store    = vdoMinAlbireoTimerIntervalStore,
  .valuePtr = &minAlbireoTimerInterval,
};

static VDOAttribute vdoTraceRecording = {
  .attr     = {.name = "trace_recording", .mode = 0644, },
  .show     = showBool,
  .store    = vdoTraceRecordingStore,
  .valuePtr = &traceRecording,
};

static VDOAttribute vdoVersionAttr = {
  .attr  = { .name = "version", .mode = 0444, },
  .show  = vdoVersionShow,
};

static struct attribute *defaultAttrs[] = {
  &vdoStatusAttr.attr,
  &vdoLogLevelAttr.attr,
  &vdoMaxReqActiveAttr.attr,
  &vdoAlbireoTimeoutInterval.attr,
  &vdoMinAlbireoTimerInterval.attr,
  &vdoTraceRecording.attr,
  &vdoVersionAttr.attr,
  NULL
};

static struct sysfs_ops vdoSysfsOps = {
  .show  = vdoAttrShow,
  .store = vdoAttrStore,
};

/**********************************************************************/
static void vdoRelease(struct kobject *kobj)
{
  return;
}

struct kobj_type vdo_ktype = {
  .release   = vdoRelease,
  .sysfs_ops = &vdoSysfsOps,
  .default_attrs = defaultAttrs,
};

/**********************************************************************/
int vdoInitSysfs(struct kobject *moduleObject)
{
  kobject_init(moduleObject, &vdo_ktype);
  int result = kobject_add(moduleObject, NULL, THIS_MODULE->name);
  if (result < 0) {
    logError("kobject_add failed with status %d", -result);
    kobject_put(moduleObject);
  }
  logDebug("added sysfs objects");
  return result;
};

/**********************************************************************/
void vdoPutSysfs(struct kobject *moduleObject)
{
  kobject_put(moduleObject);
}
