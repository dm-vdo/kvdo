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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/sysfs.c#2 $
 */

#include "sysfs.h"

#include <linux/module.h>
#include <linux/version.h>

#include "dedupeIndex.h"
#include "dmvdo.h"
#include "logger.h"

#define HAS_MAX_DISCARD_SECTORS (LINUX_VERSION_CODE < KERNEL_VERSION(4,3,0))

#if HAS_MAX_DISCARD_SECTORS
extern unsigned int maxDiscardSectors;
#endif /* HAS_MAX_DISCARD_SECTORS */

extern int defaultMaxRequestsActive;

typedef struct vdoAttribute {
  struct attribute  attr;
  ssize_t (*show)(struct kvdoDevice *d, struct attribute *attr, char *buf);
  ssize_t (*store)(struct kvdoDevice *d, const char *value, size_t count);
  // Location of value, if .show == showInt or showUInt or showBool.
  void     *valuePtr;
} VDOAttribute;

static char *statusStrings[] = {
  "UNINITIALIZED",
  "READY",
  "SHUTTING DOWN",
};

/**********************************************************************/
static ssize_t vdoStatusShow(struct kvdoDevice *device,
                             struct attribute  *attr,
                             char              *buf)
{
  return sprintf(buf, "%s\n", statusStrings[device->status]);
}

/**********************************************************************/
static ssize_t vdoLogLevelShow(struct kvdoDevice *device,
                               struct attribute  *attr,
                               char              *buf)
{
  return sprintf(buf, "%s\n", priorityToString(getLogLevel()));
}

/**********************************************************************/
static ssize_t vdoLogLevelStore(struct kvdoDevice *device,
                                const char *buf, size_t n)
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
static ssize_t showInt(struct kvdoDevice *device,
                       struct attribute  *attr,
                       char              *buf)
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
static ssize_t showUInt(struct kvdoDevice *device,
                        struct attribute  *attr,
                        char              *buf)
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
static ssize_t showBool(struct kvdoDevice *device,
                        struct attribute  *attr,
                        char              *buf)
{
  VDOAttribute *vdoAttr = container_of(attr, VDOAttribute, attr);

  return sprintf(buf, "%u\n", *(bool *)vdoAttr->valuePtr ? 1 : 0);
}

/**********************************************************************/
static ssize_t vdoTraceRecordingStore(struct kvdoDevice *device,
                                      const char        *buf,
                                      size_t             n)
{
  return scanBool(buf, n, &traceRecording);
}

/**********************************************************************/
static ssize_t vdoMaxReqActiveStore(struct kvdoDevice *device,
                                    const char        *buf,
                                    size_t             n)
{
  /*
   * The base code has some hardcoded assumptions about the maximum
   * number of requests that can be in progress. Maybe someday we'll
   * do calculations with the actual number; for now, just make sure
   * the assumption holds.
   */
  return scanInt(buf, n, &defaultMaxRequestsActive, 1, MAXIMUM_USER_VIOS);
}

#if HAS_MAX_DISCARD_SECTORS
/**********************************************************************/
static ssize_t vdoMaxDiscardSectors(struct kvdoDevice *device,
                                    const char        *buf,
                                    size_t             n)
{
  return scanUInt(buf, n, &maxDiscardSectors, 8, UINT_MAX);
}
#endif /* HAS_MAX_DISCARD_SECTORS */

/**********************************************************************/
static ssize_t vdoAlbireoTimeoutIntervalStore(struct kvdoDevice *device,
                                              const char        *buf,
                                              size_t             n)
{
  unsigned int value;
  ssize_t result = scanUInt(buf, n, &value, 0, UINT_MAX);
  if (result > 0) {
    setAlbireoTimeoutInterval(value);
  }
  return result;
}

/**********************************************************************/
static ssize_t vdoMinAlbireoTimerIntervalStore(struct kvdoDevice *device,
                                               const char        *buf,
                                               size_t             n)
{
  unsigned int value;
  ssize_t result = scanUInt(buf, n, &value, 0, UINT_MAX);
  if (result > 0) {
    setMinAlbireoTimerInterval(value);
  }
  return result;
}

/**********************************************************************/
static ssize_t vdoVersionShow(struct kvdoDevice *device,
                              struct attribute  *attr,
                              char              *buf)
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

  struct kvdoDevice *device = container_of(kobj, struct kvdoDevice, kobj);
  return (*vdoAttr->show)(device, attr, buf);
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

  struct kvdoDevice *device = container_of(kobj, struct kvdoDevice, kobj);
  return (*vdoAttr->store)(device, buf, length);
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

#if HAS_MAX_DISCARD_SECTORS
static VDOAttribute vdoMaxDiscardSectorsAttr = {
  .attr     = {.name = "max_discard_sectors", .mode = 0644, },
  .show     = showUInt,
  .store    = vdoMaxDiscardSectors,
  .valuePtr = &maxDiscardSectors,
};
#endif /* HAS_MAX_DISCARD_SECTORS */

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
#if HAS_MAX_DISCARD_SECTORS
  &vdoMaxDiscardSectorsAttr.attr,
#endif /* HAS_MAX_DISCARD_SECTORS */
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
int vdoInitSysfs(struct kobject *deviceObject)
{
  kobject_init(deviceObject, &vdo_ktype);
  int result = kobject_add(deviceObject, NULL, THIS_MODULE->name);
  if (result < 0) {
    logError("kobject_add failed with status %d", -result);
    kobject_put(deviceObject);
  }
  logDebug("added sysfs objects");
  return result;
};

/**********************************************************************/
void vdoPutSysfs(struct kobject *deviceObject)
{
  kobject_put(deviceObject);
}
